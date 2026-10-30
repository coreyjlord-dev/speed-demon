// Track B. The queries name every term worth indexing.
//
// Common terms are bitmaps. The rest are 16-bit document offsets, filled from
// a 3-byte scan log that is dropped bucket by bucket. Phrases are not stored
// as positions: a candidate document is checked against its raw text. BOOL is
// strictly left to right, and a bitmap result stays a bitmap.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <malloc.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using u32 = uint32_t;
using u64 = uint64_t;

constexpr u32 kNone = 0xffffffffu;
constexpr int kMaxTerm = 32;
constexpr int kLag = 8;
constexpr u32 kIdBits = 18;
constexpr u32 kIdMask = (1u << kIdBits) - 1u;
constexpr u32 kTermBits = 8;
constexpr u32 kDocBits = 16;
constexpr u32 kBucket = 1u << kTermBits;

[[noreturn]] void die(const char* m) {
  std::fprintf(stderr, "track-b: %s\n", m);
  std::exit(1);
}

int thread_count() {
  if (const char* e = std::getenv("SD_THREADS")) {
    int n = std::atoi(e);
    if (n >= 1 && n <= 32) return n;
  }
  unsigned hc = std::thread::hardware_concurrency();
  if (hc == 0) hc = 4;
  return static_cast<int>(std::min(12u, std::max(8u, hc)));
}

bool is_token(unsigned char c) {
  unsigned char f = static_cast<unsigned char>(c | 0x20u);
  return (f >= 'a' && f <= 'z') || (c >= '0' && c <= '9');
}

struct FileMap {
  const uint8_t* data = nullptr;
  size_t size = 0;
  explicit FileMap(const char* path) {
    int fd = ::open(path, O_RDONLY);
    if (fd < 0) die("open");
    struct stat st {};
    if (fstat(fd, &st) != 0) die("fstat");
    size = static_cast<size_t>(st.st_size);
    void* reserve = ::mmap(nullptr, size + 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reserve == MAP_FAILED) die("mmap");
    if (size && ::mmap(reserve, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED)
      die("mmap file");
    data = static_cast<const uint8_t*>(reserve);
    ::close(fd);
  }
};

// Fold a token with one or two word loads at the moment the tokenizer emits it.
// Bytes past n are masked off. The file map has a guard page, so the loads stay in range.
inline void words_of(const uint8_t* s, u32 n, u64& w0, u64& w1) {
  u64 a = 0;
  std::memcpy(&a, s, 8);
  a |= 0x2020202020202020ull;
  if (n <= 8) {
    if (n < 8) a &= (1ull << (8 * n)) - 1;
    w0 = a;
    w1 = 0;
    return;
  }
  u64 b = 0;
  std::memcpy(&b, s + 8, 8);
  b |= 0x2020202020202020ull;
  if (n < 16) b &= (1ull << (8 * (n - 8))) - 1;
  w0 = a;
  w1 = b;
}

inline u64 mix_tok(u64 w0, u64 w1, u32 n) {
  u64 h = w0 + 0x9E3779B185EBCA87ull;
  h ^= w1 + (static_cast<u64>(n) << 1);
  h *= 0xC2B2AE3D27D4EB4Full;
  h ^= h >> 33;
  h *= 0x165667B19E3779F9ull;
  return h;
}

inline bool tail_eq(const uint8_t* a, const uint8_t* s, u32 n) {
  for (u32 i = 16; i < n; i++)
    if (a[i] != (s[i] | 0x20u)) return false;
  return true;
}

inline bool tail_eq_raw(const uint8_t* a, const uint8_t* s, u32 n) {
  for (u32 i = 16; i < n; i++)
    if ((a[i] | 0x20u) != (s[i] | 0x20u)) return false;
  return true;
}

struct Rec {
  u64 w0 = 0, w1 = 0;
  u32 off = 0;
  uint8_t n = 0;
};

// 32-bit slots, sized for L2. Low 18 bits are id+1, high 14 are a tag.
// Load stays under one half, so the home slot is the common case.
struct Terms {
  static constexpr size_t kCap = 1u << 18;
  std::vector<u32> slot;
  std::vector<Rec> rec;
  std::vector<char> text;
  size_t mask = 0;

  u32 size() const { return static_cast<u32>(rec.size()); }
  const uint8_t* bytes(u32 id) const { return reinterpret_cast<const uint8_t*>(text.data() + rec[id].off); }

  static u32 tag_of(u64 h) { return static_cast<u32>(h >> 50); }
  static u32 pack(u32 tag, u32 id) { return (tag << kIdBits) | (id + 1u); }

  void rehash(size_t cap) {
    slot.assign(cap, 0);
    mask = cap - 1;
    for (u32 id = 0; id < rec.size(); id++) insert_slot(id);
  }

  void grow() {
    if (rec.size() >= kIdMask) die("term id");
    if (!slot.empty() && (rec.size() + 1) * 2 <= slot.size()) return;
    rehash(slot.empty() ? kCap : slot.size() * 2);
  }

  void insert_slot(u32 id) {
    u64 h = mix_tok(rec[id].w0, rec[id].w1, rec[id].n);
    u32 tag = tag_of(h);
    for (size_t i = h & mask;; i = (i + 1) & mask) {
      if (!slot[i]) {
        slot[i] = pack(tag, id);
        return;
      }
    }
  }

  bool eq_at(u32 id, u64 w0, u64 w1, u32 n, const uint8_t* s) const {
    const Rec& r = rec[id];
    if (r.n != n || r.w0 != w0 || (n > 8 && r.w1 != w1)) return false;
    return n <= 16 || tail_eq(reinterpret_cast<const uint8_t*>(text.data() + r.off), s, n);
  }

  u32 add(const uint8_t* s, u32 n) {
    grow();
    u64 w0, w1;
    words_of(s, n, w0, w1);
    u64 h = mix_tok(w0, w1, n);
    u32 tag = tag_of(h);
    for (size_t i = h & mask;; i = (i + 1) & mask) {
      u32 sl = slot[i];
      if (!sl) {
        Rec r;
        r.w0 = w0;
        r.w1 = w1;
        r.n = static_cast<uint8_t>(n);
        r.off = static_cast<u32>(text.size());
        text.resize(text.size() + n);
        for (u32 k = 0; k < n; k++) text[r.off + k] = static_cast<char>(s[k] | 0x20u);
        rec.push_back(r);
        slot[i] = pack(tag, static_cast<u32>(rec.size() - 1));
        return static_cast<u32>(rec.size() - 1);
      }
      if ((sl >> kIdBits) == tag && eq_at((sl & kIdMask) - 1u, w0, w1, n, s))
        return (sl & kIdMask) - 1u;
    }
  }

  void renumber(const std::vector<u32>& neu) {
    std::vector<Rec> nr(rec.size());
    for (u32 i = 0; i < rec.size(); i++) nr[neu[i]] = rec[i];
    rec.swap(nr);
    rehash(std::max(slot.size(), kCap));
  }

  void prefetch(u64 h) const { _mm_prefetch(reinterpret_cast<const char*>(&slot[h & mask]), _MM_HINT_T0); }

  // Home slot first. The table stays under half full, so this returns immediately
  // for a miss and for most hits.
  u32 find_fast(u64 w0, u64 w1, u32 n, u64 h, const uint8_t* s) const {
    u32 tag = tag_of(h);
    for (size_t i = h & mask;; i = (i + 1) & mask) {
      u32 sl = slot[i];
      if (!sl) return kNone;
      if ((sl >> kIdBits) == tag && eq_at((sl & kIdMask) - 1u, w0, w1, n, s))
        return (sl & kIdMask) - 1u;
    }
  }
};

// Distinct tokens in the current document. A repeat skips the term-table probe.
// 4096 covers a generated document (measured maximum is under 800 distinct tokens).
struct DocSeen {
  struct E {
    u64 w0 = 0, w1 = 0;
    const uint8_t* s = nullptr;
    u32 n = 0, gen = 0;
  };
  static constexpr u32 kM = 4096;
  E tab[kM];
  u32 gen = 1;

  void next() {
    if (++gen == 0) {
      std::memset(tab, 0, sizeof tab);
      gen = 1;
    }
  }

  bool claim(u64 h, u64 w0, u64 w1, u32 n, const uint8_t* s) {
    u32 i = static_cast<u32>(h) & (kM - 1);
    for (u32 k = 0; k < kM; k++) {
      E& e = tab[i];
      if (e.gen != gen) {
        e.w0 = w0;
        e.w1 = w1;
        e.n = n;
        e.s = s;
        e.gen = gen;
        return true;
      }
      if (e.w0 == w0 && e.n == n && (n <= 8 || e.w1 == w1) &&
          (n <= 16 || tail_eq_raw(e.s, s, n)))
        return false;
      i = (i + 1) & (kM - 1);
    }
    die("doc table");
  }
};

inline u32 token_mask32(const uint8_t* p) {
  __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
  __m256i low = _mm256_or_si256(v, _mm256_set1_epi8(0x20));
  __m256i letters = _mm256_sub_epi8(low, _mm256_set1_epi8('a'));
  __m256i digits = _mm256_sub_epi8(v, _mm256_set1_epi8('0'));
  __m256i zero = _mm256_setzero_si256();
  __m256i in = _mm256_or_si256(_mm256_cmpeq_epi8(_mm256_subs_epu8(letters, _mm256_set1_epi8(25)), zero),
                              _mm256_cmpeq_epi8(_mm256_subs_epu8(digits, _mm256_set1_epi8(9)), zero));
  return static_cast<u32>(_mm256_movemask_epi8(in));
}

template <class F>
void for_tokens(const uint8_t* q, const uint8_t* e, const Terms& terms, DocSeen& seen, F&& emit) {
  struct P {
    const uint8_t* s;
    u32 n;
    u64 w0, w1, h;
  };
  // Two groups. Filling one hides the prefetches issued for the other.
  P buf[2][kLag];
  int cur = 0, cn = 0, prev = -1, pn = 0;
  auto probe = [&](int g, int n) {
    for (int i = 0; i < n; i++) {
      const P& t = buf[g][i];
      emit(terms.find_fast(t.w0, t.w1, t.n, t.h, t.s));
    }
  };
  auto push = [&](const uint8_t* s, u32 len) {
    if (len == 0 || len > kMaxTerm) return;
    u64 w0, w1;
    words_of(s, len, w0, w1);
    u64 h = mix_tok(w0, w1, len);
    if (!seen.claim(h, w0, w1, len, s)) return;
    if (cn == kLag) {
      for (int i = 0; i < cn; i++) terms.prefetch(buf[cur][i].h);
      if (prev >= 0) probe(prev, pn);
      prev = cur;
      pn = cn;
      cur ^= 1;
      cn = 0;
    }
    buf[cur][cn++] = P{s, len, w0, w1, h};
  };
  const uint8_t* tok = nullptr;
  auto flush = [&](const uint8_t* end) {
    if (!tok) return;
    push(tok, static_cast<u32>(end - tok));
    tok = nullptr;
  };
  const uint8_t* p = q;
  while (p + 32 <= e) {
    u32 m = token_mask32(p);
    if (m == 0) {
      flush(p);
      p += 32;
      continue;
    }
    if (m == 0xffffffffu) {
      if (!tok) tok = p;
      p += 32;
      continue;
    }
    u32 bit = 0;
    if (tok) {
      if (m & 1u) {
        u32 run = static_cast<u32>(__builtin_ctz(~m));
        flush(p + run);
        bit = run;
      } else {
        flush(p);
      }
    }
    while (bit < 32) {
      u32 rest = m >> bit;
      if (!rest) break;
      bit += static_cast<u32>(__builtin_ctz(rest));
      if (bit >= 32) break;
      rest = m >> bit;
      u32 ones = static_cast<u32>(__builtin_ctz(~rest));
      if (bit + ones >= 32) {
        tok = p + bit;
        break;
      }
      push(p + bit, ones);
      bit += ones;
    }
    p += 32;
  }
  while (p < e) {
    if (is_token(*p)) {
      if (!tok) tok = p;
    } else if (tok) {
      flush(p);
    }
    ++p;
  }
  flush(e);
  if (cn)
    for (int i = 0; i < cn; i++) terms.prefetch(buf[cur][i].h);
  if (prev >= 0) probe(prev, pn);
  if (cn) probe(cur, cn);
}

struct BlockLog {
  static constexpr size_t kBlock = 1 << 18;
  static constexpr size_t kBytes = 3 * kBlock + 4;
  std::vector<uint8_t*> blocks;
  uint8_t* cur = nullptr;
  size_t fill = kBlock;

  void push(u32 x) {
    if (fill == kBlock) {
      cur = static_cast<uint8_t*>(
          ::mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
      if (cur == MAP_FAILED) die("mmap log");
      blocks.push_back(cur);
      fill = 0;
    }
    std::memcpy(cur + 3 * fill, &x, 4);
    fill++;
  }

  template <class F>
  void drain(F&& fn) {
    for (size_t b = 0; b < blocks.size(); b++) {
      size_t n = b + 1 == blocks.size() ? fill : kBlock;
      for (size_t i = 0; i < n; i++) {
        u32 x = 0;
        std::memcpy(&x, blocks[b] + 3 * i, 4);
        fn(x & 0xFFFFFFu);
      }
      ::munmap(blocks[b], kBytes);
    }
    blocks.clear();
  }

  ~BlockLog() {
    for (uint8_t* b : blocks) ::munmap(b, kBytes);
  }
};

struct Index {
  u32 ndocs = 0;
  u32 words = 0;
  int threads = 0;
  std::vector<u32> dense;
  std::vector<u64> bits;
  std::vector<u32> df;
  std::vector<u32> origin;
  std::vector<u32> start;
  std::vector<std::vector<u32>> seg_end;
  uint16_t* post = nullptr;
  size_t post_n = 0;
  std::vector<u32> text;
  ~Index() {
    if (post) ::munmap(post, post_n * sizeof(uint16_t));
  }
};

u32 parse_id(const uint8_t* p) {
  u32 v = 0;
  while (*p >= '0' && *p <= '9') v = v * 10u + static_cast<u32>(*p++ - '0');
  return v;
}

struct Qry {
  bool phrase = false;
  u32 begin = 0;
  u32 n = 0;
  u32 op_at = 0;
};

Index build(const FileMap& file, Terms& terms, std::vector<u32>& qterms, int T) {
  Index ix;
  const uint8_t* base = file.data;
  const size_t size = file.size;
  if (size) {
    size_t last = size - 1;
    while (last > 0 && base[last - 1] != '\n') --last;
    ix.ndocs = parse_id(base + last) + 1;
  }
  ix.threads = T;
  ix.words = (ix.ndocs + 63) / 64;
  ix.origin.resize(static_cast<size_t>(T));
  ix.text.assign(static_cast<size_t>(ix.ndocs) * 2, 0);
  const u32 nterms = terms.size();
  ix.dense.assign(nterms, kNone);
  const u32 nbuckets = (nterms + kBucket - 1) / kBucket;

  std::vector<size_t> cut(static_cast<size_t>(T) + 1);
  for (int t = 0; t <= T; t++) {
    size_t c = size * static_cast<size_t>(t) / static_cast<size_t>(T);
    while (c > 0 && c < size && base[c - 1] != '\n') ++c;
    cut[t] = c;
  }

  std::vector<std::vector<u32>> count(static_cast<size_t>(T));
  std::vector<u32> sampled(static_cast<size_t>(T));
  std::vector<std::thread> pool;
  for (int t = 0; t < T; t++) {
    pool.emplace_back([&, t] {
      const uint8_t* begin = base + cut[t];
      const uint8_t* end = base + cut[t + 1];
      u32 first = begin < end ? parse_id(begin) : 0;
      ix.origin[t] = first;
      auto& cnt = count[t];
      cnt.assign(nterms, 0);
      DocSeen doc_seen;
      const uint8_t* stop = begin + (end - begin) / 32;
      u32 doc = first;
      for (const uint8_t* p = begin; p < stop && p < end;) {
        const uint8_t* nl = static_cast<const uint8_t*>(std::memchr(p, '\n', end - p));
        const uint8_t* tab = static_cast<const uint8_t*>(std::memchr(p, '\t', nl - p));
        doc_seen.next();
        for_tokens(tab + 1, nl, terms, doc_seen, [&](u32 id) {
          if (id >= nterms) return;
          cnt[id]++;
        });
        p = nl + 1;
        doc++;
      }
      sampled[t] = doc - first;
    });
  }
  for (auto& th : pool) th.join();
  pool.clear();

  u64 sample_docs = 0;
  std::vector<u64> sdf(nterms);
  for (int t = 0; t < T; t++) {
    sample_docs += sampled[t];
    for (u32 id = 0; id < nterms; id++) sdf[id] += count[t][id];
  }
  std::vector<u32> order(nterms), neu(nterms);
  for (u32 id = 0; id < nterms; id++) order[id] = id;
  std::stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) { return sdf[a] > sdf[b]; });
  u32 ndense = 0;
  std::vector<u32> dense_new(nterms, kNone);
  for (u32 rank = 0; rank < nterms; rank++) {
    neu[order[rank]] = rank;
    if (sdf[order[rank]] * 32 > sample_docs) dense_new[rank] = ndense++;
  }
  terms.renumber(neu);
  for (u32& id : qterms) id = neu[id];
  ix.dense.swap(dense_new);
  ix.bits.assign(static_cast<size_t>(ndense) * ix.words, 0);

  std::vector<std::vector<BlockLog>> logs(static_cast<size_t>(T));
  for (int t = 0; t < T; t++) count[t].assign(nterms, 0);
  for (int t = 0; t < T; t++) {
    pool.emplace_back([&, t] {
      const uint8_t* begin = base + cut[t];
      const uint8_t* end = base + cut[t + 1];
      u32 first = ix.origin[t];
      auto& cnt = count[t];
      DocSeen doc_seen;
      auto& out = logs[t];
      out.resize(nbuckets);
      u32 doc = first;
      u32 nd = 0;
      for (const uint8_t* p = begin; p < end;) {
        const uint8_t* nl = static_cast<const uint8_t*>(std::memchr(p, '\n', end - p));
        const uint8_t* tab = static_cast<const uint8_t*>(std::memchr(p, '\t', nl - p));
        const uint8_t* tx = tab + 1;
        ix.text[2 * doc] = static_cast<u32>(tx - base);
        ix.text[2 * doc + 1] = static_cast<u32>(nl - base);
        doc_seen.next();
        for_tokens(tx, nl, terms, doc_seen, [&](u32 id) {
          if (id >= nterms) return;
          if (u32 d = ix.dense[id]; d != kNone) {
            __atomic_or_fetch(&ix.bits[static_cast<size_t>(d) * ix.words + (doc >> 6)],
                              1ull << (doc & 63), __ATOMIC_RELAXED);
          } else {
            u32 rel = doc - first;
            out[id >> kTermBits].push(((id & (kBucket - 1)) << kDocBits) | rel);
            cnt[id]++;
          }
        });
        p = nl + 1;
        doc++;
        nd++;
      }
      if (nd >= (1u << kDocBits)) die("doc offset overflow");
    });
  }
  for (auto& th : pool) th.join();
  pool.clear();

  ix.start.resize(nterms + 1);
  u32 total = 0;
  for (u32 id = 0; id < nterms; id++) {
    ix.start[id] = total;
    for (int t = 0; t < T; t++) {
      u32 c = count[t][id];
      count[t][id] = total;
      total += c;
    }
  }
  ix.start[nterms] = total;
  ix.post_n = total;
  void* mem = ::mmap(nullptr, std::max<size_t>(total, 1) * sizeof(uint16_t), PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED) die("mmap post");
  ix.post = static_cast<uint16_t*>(mem);
  for (int t = 0; t < T; t++) {
    pool.emplace_back([&, t] {
      for (u32 b = 0; b < nbuckets; b++) {
        logs[t][b].drain([&](u32 x) {
          u32 id = (b << kTermBits) | (x >> kDocBits);
          ix.post[count[t][id]++] = static_cast<uint16_t>(x & ((1u << kDocBits) - 1));
        });
      }
    });
  }
  for (auto& th : pool) th.join();
  ix.seg_end = std::move(count);
  ix.df.assign(nterms, 0);
  for (u32 id = 0; id < nterms; id++) {
    if (ix.dense[id] != kNone) {
      u32 pc = 0;
      const u64* b = ix.bits.data() + static_cast<size_t>(ix.dense[id]) * ix.words;
      for (u32 w = 0; w < ix.words; w++) pc += static_cast<u32>(__builtin_popcountll(b[w]));
      ix.df[id] = pc;
    } else {
      ix.df[id] = ix.start[id + 1] - ix.start[id];
    }
  }
  return ix;
}

struct Post {
  const u32* p = nullptr;
  size_t n = 0;
  const u64* bits = nullptr;
};

size_t gallop(const u32* a, size_t i, size_t n, u32 x) {
  size_t step = 1, hi = i;
  while (hi < n && a[hi] < x) {
    i = hi + 1;
    hi += step;
    if (step < (1u << 30)) step <<= 1;
  }
  if (hi > n) hi = n;
  return static_cast<size_t>(std::lower_bound(a + i, a + hi, x) - a);
}

void op_and(const std::vector<u32>& a, const Post& b, std::vector<u32>& out) {
  out.clear();
  if (!b.n) return;
  if (b.bits) {
    for (u32 x : a)
      if ((b.bits[x >> 6] >> (x & 63)) & 1ull) out.push_back(x);
    return;
  }
  if (a.size() * 16 < b.n) {
    size_t j = 0;
    for (u32 x : a) {
      j = gallop(b.p, j, b.n, x);
      if (j == b.n) break;
      if (b.p[j] == x) out.push_back(x);
    }
  } else if (b.n * 16 < a.size()) {
    size_t i = 0;
    for (size_t k = 0; k < b.n; k++) {
      i = gallop(a.data(), i, a.size(), b.p[k]);
      if (i == a.size()) break;
      if (a[i] == b.p[k]) out.push_back(b.p[k]);
    }
  } else {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.n) {
      if (a[i] == b.p[j]) {
        out.push_back(a[i]);
        ++i;
        ++j;
      } else if (a[i] < b.p[j]) ++i;
      else ++j;
    }
  }
}

void op_or(const std::vector<u32>& a, const Post& b, std::vector<u32>& out) {
  out.clear();
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.n) {
    if (a[i] == b.p[j]) {
      out.push_back(a[i]);
      ++i;
      ++j;
    } else if (a[i] < b.p[j]) out.push_back(a[i++]);
    else out.push_back(b.p[j++]);
  }
  while (i < a.size()) out.push_back(a[i++]);
  while (j < b.n) out.push_back(b.p[j++]);
}

void op_not(const std::vector<u32>& a, const Post& b, std::vector<u32>& out) {
  out.clear();
  if (!b.n) {
    out = a;
    return;
  }
  if (b.bits) {
    for (u32 x : a)
      if (((b.bits[x >> 6] >> (x & 63)) & 1ull) == 0) out.push_back(x);
    return;
  }
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.n) {
    if (a[i] == b.p[j]) {
      ++i;
      ++j;
    } else if (a[i] < b.p[j]) out.push_back(a[i++]);
    else ++j;
  }
  while (i < a.size()) out.push_back(a[i++]);
}

u64 bit_index_sum(u64 m) {
  static constexpr u64 kSel[6] = {0xAAAAAAAAAAAAAAAAull, 0xCCCCCCCCCCCCCCCCull, 0xF0F0F0F0F0F0F0F0ull,
                                  0xFF00FF00FF00FF00ull, 0xFFFF0000FFFF0000ull, 0xFFFFFFFF00000000ull};
  u64 s = 0;
  for (int k = 0; k < 6; k++) s += static_cast<u64>(__builtin_popcountll(m & kSel[k])) << k;
  return s;
}

struct Engine {
  const Index* ix = nullptr;
  const Terms* terms = nullptr;
  const FileMap* docs = nullptr;
  const std::vector<u32>* qterms = nullptr;
  const std::vector<char>* qops = nullptr;
  std::vector<u32> cur, tmp, list, order;
  std::vector<u64> bm;
  bool dense = false;

  Post decode(u32 id) {
    Post r;
    if (u32 d = ix->dense[id]; d != kNone) {
      r.bits = ix->bits.data() + static_cast<size_t>(d) * ix->words;
      r.n = ix->df[id];
      return r;
    }
    list.resize(ix->df[id]);
    u32 i = ix->start[id];
    size_t n = 0;
    for (int t = 0; t < ix->threads; t++) {
      u32 e = ix->seg_end[t][id];
      u32 org = ix->origin[t];
      for (; i < e; i++) list[n++] = org + ix->post[i];
    }
    r.p = list.data();
    r.n = n;
    return r;
  }

  void boolean(u32 begin, u32 n, u32 op_at) {
    Post first = decode(qterms->operator[](begin));
    dense = first.bits != nullptr;
    if (dense) bm.assign(first.bits, first.bits + ix->words);
    else cur.assign(first.p, first.p + first.n);
    for (u32 k = 1; k < n; k++) {
      char op = qops->operator[](op_at + (k - 1));
      Post b = decode((*qterms)[begin + k]);
      if (dense) apply_dense(op, b);
      else if (b.bits && op == 'O') {
        bm.assign(b.bits, b.bits + ix->words);
        for (u32 x : cur) bm[x >> 6] |= 1ull << (x & 63);
        dense = true;
      } else {
        if (op == 'A') op_and(cur, b, tmp);
        else if (op == 'O') op_or(cur, b, tmp);
        else op_not(cur, b, tmp);
        cur.swap(tmp);
      }
    }
  }

  void apply_dense(char op, const Post& b) {
    if (b.bits) {
      if (op == 'A')
        for (u32 w = 0; w < ix->words; w++) bm[w] &= b.bits[w];
      else if (op == 'O')
        for (u32 w = 0; w < ix->words; w++) bm[w] |= b.bits[w];
      else
        for (u32 w = 0; w < ix->words; w++) bm[w] &= ~b.bits[w];
      return;
    }
    if (op == 'A') {
      cur.clear();
      for (size_t i = 0; i < b.n; i++)
        if ((bm[b.p[i] >> 6] >> (b.p[i] & 63)) & 1ull) cur.push_back(b.p[i]);
      dense = false;
      return;
    }
    for (size_t i = 0; i < b.n; i++) {
      u32 id = b.p[i];
      if (op == 'O') bm[id >> 6] |= 1ull << (id & 63);
      else bm[id >> 6] &= ~(1ull << (id & 63));
    }
  }

  bool token_is(const uint8_t* p, size_t n, u32 id) const {
    if (n != terms->rec[id].n) return false;
    const uint8_t* k = terms->bytes(id);
    for (size_t i = 0; i < n; i++)
      if ((p[i] | 0x20) != k[i]) return false;
    return true;
  }

  bool around(const uint8_t* b, const uint8_t* e, const uint8_t* s, const uint8_t* t,
              const u32* phrase, u32 pn, u32 j) const {
    for (u32 i = j; i-- > 0;) {
      while (s > b && !is_token(s[-1])) --s;
      const uint8_t* end = s;
      while (s > b && is_token(s[-1])) --s;
      if (s == end || !token_is(s, static_cast<size_t>(end - s), phrase[i])) return false;
    }
    for (u32 i = j + 1; i < pn; i++) {
      while (t < e && !is_token(*t)) ++t;
      const uint8_t* st = t;
      while (t < e && is_token(*t)) ++t;
      if (t == st || !token_is(st, static_cast<size_t>(t - st), phrase[i])) return false;
    }
    return true;
  }

  bool contains(const uint8_t* b, const uint8_t* e, const u32* phrase, u32 pn, u32 j) const {
    u32 anchor = phrase[j];
    uint8_t first = terms->bytes(anchor)[0];
    const __m256i want = _mm256_set1_epi8(static_cast<char>(first));
    const __m256i fold = _mm256_set1_epi8(0x20);
    for (const uint8_t* p = b; p < e;) {
      u32 m = 0;
      int room = static_cast<int>(e - p);
      int step = room >= 32 ? 32 : room;
      if (room >= 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(p));
        m = static_cast<u32>(_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_or_si256(v, fold), want)));
      } else {
        for (int i = 0; i < room; i++) m |= static_cast<u32>(((p[i] | 0x20) == first) << i);
      }
      while (m) {
        const uint8_t* s = p + __builtin_ctz(m);
        m &= m - 1;
        if (s > b && is_token(s[-1])) continue;
        const uint8_t* t = s;
        while (t < e && is_token(*t)) ++t;
        if (token_is(s, static_cast<size_t>(t - s), anchor) && around(b, e, s, t, phrase, pn, j))
          return true;
      }
      p += step;
    }
    return false;
  }

  void phrase(u32 begin, u32 n) {
    dense = false;
    order.clear();
    const u32* ph = qterms->data() + begin;
    for (u32 i = 0; i < n; i++) order.push_back(ph[i]);
    std::sort(order.begin(), order.end());
    order.erase(std::unique(order.begin(), order.end()), order.end());
    std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return ix->df[a] < ix->df[b]; });
    if (order.empty() || ix->df[order[0]] == 0) {
      cur.clear();
      return;
    }
    Post a = decode(order[0]);
    if (a.bits) {
      cur.clear();
      for (u32 w = 0; w < ix->words; w++) {
        u64 x = a.bits[w];
        while (x) {
          u32 b = static_cast<u32>(__builtin_ctzll(x));
          u32 id = w * 64u + b;
          if (id >= ix->ndocs) break;
          cur.push_back(id);
          x &= x - 1;
        }
      }
    } else {
      cur.assign(a.p, a.p + a.n);
    }
    for (size_t i = 1; i < order.size() && !cur.empty(); i++) {
      op_and(cur, decode(order[i]), tmp);
      cur.swap(tmp);
    }
    u32 j = 0;
    while (j < n && ph[j] != order[0]) j++;
    size_t kept = 0;
    for (u32 doc : cur) {
      const uint8_t* b = docs->data + ix->text[2 * doc];
      const uint8_t* e = docs->data + ix->text[2 * doc + 1];
      if (contains(b, e, ph, n, j)) cur[kept++] = doc;
    }
    cur.resize(kept);
  }

  int format(char* out) const {
    u64 count = 0, sum = 0;
    u32 small[10];
    size_t m = 0;
    if (dense) {
      for (u32 w = 0; w < ix->words; w++) {
        u64 bits = bm[w];
        u64 c = static_cast<u64>(__builtin_popcountll(bits));
        count += c;
        sum += c * 64ull * w + bit_index_sum(bits);
        for (; bits && m < 10; bits &= bits - 1) {
          u32 id = w * 64u + static_cast<u32>(__builtin_ctzll(bits));
          if (id >= ix->ndocs) break;
          small[m++] = id;
        }
      }
    } else {
      count = cur.size();
      for (u32 x : cur) sum += x;
      for (; m < count && m < 10; m++) small[m] = cur[m];
    }
    auto put = [](char* p, u64 v) {
      char tmp[24];
      int n = 0;
      do tmp[n++] = static_cast<char>('0' + v % 10), v /= 10;
      while (v);
      while (n) *p++ = tmp[--n];
      return p;
    };
    char* p = put(out, count);
    *p++ = ' ';
    p = put(p, sum);
    for (size_t i = 0; i < m; i++) {
      *p++ = ' ';
      p = put(p, small[i]);
    }
    *p++ = '\n';
    return static_cast<int>(p - out);
  }
};

const char* env_or(const char* k, const char* fb) {
  const char* v = std::getenv(k);
  return v && *v ? v : fb;
}

}  // namespace

int main() {
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
  mallopt(M_MMAP_THRESHOLD, 1 << 16);
  mallopt(M_TRIM_THRESHOLD, 1 << 16);
  mallopt(M_TOP_PAD, 0);
  std::string dir = env_or("DATA", "/data");
  std::string out_path = std::string(env_or("OUT", "/out")) + "/results.txt";
  FileMap qf((dir + "/queries.txt").c_str());
  FileMap df((dir + "/docs.txt").c_str());

  Terms terms;
  std::vector<Qry> queries;
  std::vector<u32> qterms;
  std::vector<char> qops;
  queries.reserve(50000);
  const uint8_t* p = qf.data;
  const uint8_t* qe = p + qf.size;
  while (p < qe) {
    const uint8_t* line = p;
    while (p < qe && *p != '\n') ++p;
    const uint8_t* lend = p;
    if (p < qe) ++p;
    Qry q;
    if (static_cast<size_t>(lend - line) >= 5 && std::memcmp(line, "BOOL ", 5) == 0) {
      q.begin = static_cast<u32>(qterms.size());
      q.op_at = static_cast<u32>(qops.size());
      const uint8_t* s = line + 5;
      while (s < lend) {
        const uint8_t* t = s;
        while (s < lend && *s != ' ') ++s;
        qterms.push_back(terms.add(t, static_cast<u32>(s - t)));
        if (s >= lend) break;
        ++s;
        if (s + 3 <= lend && std::memcmp(s, "AND", 3) == 0) {
          qops.push_back('A');
          s += 3;
        } else if (s + 2 <= lend && std::memcmp(s, "OR", 2) == 0) {
          qops.push_back('O');
          s += 2;
        } else if (s + 3 <= lend && std::memcmp(s, "NOT", 3) == 0) {
          qops.push_back('N');
          s += 3;
        }
        if (s < lend && *s == ' ') ++s;
      }
      q.n = static_cast<u32>(qterms.size() - q.begin);
    } else if (static_cast<size_t>(lend - line) >= 7 && std::memcmp(line, "PHRASE ", 7) == 0) {
      q.phrase = true;
      q.begin = static_cast<u32>(qterms.size());
      const uint8_t* s = line + 7;
      if (s < lend && *s == '"') ++s;
      while (s < lend && *s != '"') {
        const uint8_t* t = s;
        while (s < lend && *s != ' ' && *s != '"') ++s;
        qterms.push_back(terms.add(t, static_cast<u32>(s - t)));
        if (s < lend && *s == ' ') ++s;
      }
      q.n = static_cast<u32>(qterms.size() - q.begin);
    }
    queries.push_back(q);
  }

  int T = thread_count();
  auto t0 = std::chrono::steady_clock::now();
  Index ix = build(df, terms, qterms, T);
  malloc_trim(0);
  malloc_trim(0);
  auto t1 = std::chrono::steady_clock::now();

  constexpr int kSlot = 128;
  const size_t nq = queries.size();
  std::vector<char> slots(nq * kSlot);
  std::vector<int> lens(nq);
  std::atomic<size_t> next{0};
  std::vector<std::thread> pool;
  for (int t = 0; t < T; t++) {
    pool.emplace_back([&] {
      Engine eng;
      eng.ix = &ix;
      eng.terms = &terms;
      eng.docs = &df;
      eng.qterms = &qterms;
      eng.qops = &qops;
      for (;;) {
        size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= nq) break;
        const Qry& q = queries[i];
        if (q.phrase) eng.phrase(q.begin, q.n);
        else eng.boolean(q.begin, q.n, q.op_at);
        lens[i] = eng.format(slots.data() + i * kSlot);
      }
    });
  }
  for (auto& th : pool) th.join();

  int fd = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) die("open output");
  for (size_t i = 0; i < nq; i++) {
    const char* s = slots.data() + i * kSlot;
    size_t n = static_cast<size_t>(lens[i]);
    while (n) {
      ssize_t k = ::write(fd, s, n);
      if (k < 0) die("write");
      s += k;
      n -= static_cast<size_t>(k);
    }
  }
  ::close(fd);
  auto t2 = std::chrono::steady_clock::now();
  if (std::getenv("SD_STATS")) {
    auto ms = [](auto a, auto b) {
      return std::chrono::duration<double, std::milli>(b - a).count();
    };
    std::fprintf(stderr, "index %.0f ms queries %.0f ms threads %d\n", ms(t0, t1), ms(t1, t2), T);
  }
  return 0;
}
