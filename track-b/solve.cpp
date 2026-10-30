// Track B. Queries name every indexed term. Common terms are bitmaps; the rest
// are 16-bit document offsets filled from a 3-byte scan log. Phrases are checked
// on the raw text. BOOL is left to right, and a bitmap result stays a bitmap.
//
// IF/IB/FOR/FB/WH/WB/EL/EI/EIB/EB are the same control flow, written so a
// formatted line holds the header and the body together.

#include <algorithm>
#include <atomic>
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

using namespace std;
using u32 = uint32_t;
using u64 = uint64_t;

constexpr u32 kNone = 0xffffffffu;
constexpr int kMaxTerm = 32, kLag = 8;
constexpr u32 kIdBits = 18, kIdMask = (1u << kIdBits) - 1u;
constexpr u32 kTermBits = 8, kDocBits = 16, kBucket = 1u << kTermBits;

#define IF(c, ...) i##f(c) __VA_ARGS__
#define IB(c, ...) i##f(c) <% __VA_ARGS__; %>
#define FOR(h, ...) f##or(h) __VA_ARGS__
#define FB(h, ...) f##or(h) <% __VA_ARGS__; %>
#define WH(c, ...) w##hile(c) __VA_ARGS__
#define WB(c, ...) w##hile(c) <% __VA_ARGS__; %>
#define EL(...) e##lse __VA_ARGS__
#define EI(c, ...) e##lse i##f(c) __VA_ARGS__
#define EIB(c, ...) e##lse i##f(c) <% __VA_ARGS__; %>
#define EB(...) e##lse <% __VA_ARGS__; %>

[[noreturn]] void die(const char* m) { fprintf(stderr, "track-b: %s\n", m), exit(1); }

int thread_count() {
  IB(const char* e = getenv("SD_THREADS"), int n = atoi(e); IF(n >= 1 && n <= 32, return n));
  unsigned hc = thread::hardware_concurrency();
  IF(!hc, hc = 4);
  return (int)min(12u, max(8u, hc));
}

bool is_token(unsigned char c) {
  return ((c | 0x20) >= 'a' && (c | 0x20) <= 'z') || (c >= '0' && c <= '9');
}

const char* env_or(const char* k, const char* fb) {
  const char* v = getenv(k);
  return v && *v ? v : fb;
}

struct FileMap {
  const uint8_t* data = nullptr;
  size_t size = 0;
  explicit FileMap(const char* path) {
    int fd = ::open(path, O_RDONLY);
    IF(fd < 0, die("open"));
    struct stat st {};
    IF(fstat(fd, &st) != 0, die("fstat"));
    size = (size_t)st.st_size;
    void* reserve = mmap(nullptr, size + 4096, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    IF(reserve == MAP_FAILED, die("mmap"));
    IF(size && mmap(reserve, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0) == MAP_FAILED, die("file"));
    data = (const uint8_t*)reserve;
    ::close(fd);
  }
};

inline void words_of(const uint8_t* s, u32 n, u64& w0, u64& w1) {
  u64 a = 0;
  memcpy(&a, s, 8);
  a |= 0x2020202020202020ull;
  IB(n <= 8, IF(n < 8, a &= (1ull << (8 * n)) - 1); w0 = a; w1 = 0; return);
  u64 b = 0;
  memcpy(&b, s + 8, 8);
  b |= 0x2020202020202020ull;
  IF(n < 16, b &= (1ull << (8 * (n - 8))) - 1);
  w0 = a, w1 = b;
}

inline u64 mix_tok(u64 w0, u64 w1, u32 n) {
  u64 h = w0 + 0x9E3779B185EBCA87ull;
  h ^= w1 + ((u64)n << 1);
  h *= 0xC2B2AE3D27D4EB4Full;
  h ^= h >> 33;
  return h * 0x165667B19E3779F9ull;
}

template <bool Raw>
inline bool tail_eq(const uint8_t* a, const uint8_t* s, u32 n) {
  FOR(u32 i = 16; i < n; ++i, IF((Raw ? a[i] | 0x20u : a[i]) != (s[i] | 0x20u), return false));
  return true;
}

struct Rec {
  u64 w0 = 0, w1 = 0;
  u32 off = 0;
  uint8_t n = 0;
};

struct Terms {
  static constexpr size_t kCap = 1u << 18;
  vector<u32> slot;
  vector<Rec> rec;
  vector<char> text;
  size_t mask = 0;

  u32 size() const { return (u32)rec.size(); }
  const uint8_t* bytes(u32 id) const { return (const uint8_t*)text.data() + rec[id].off; }
  static u32 tag_of(u64 h) { return (u32)(h >> 50); }
  static u32 pack(u32 tag, u32 id) { return (tag << kIdBits) | (id + 1u); }

  void rehash(size_t cap) {
    slot.assign(cap, 0);
    mask = cap - 1;
    FOR(u32 id = 0; id < rec.size(); ++id, insert_slot(id));
  }

  void grow() {
    IF(rec.size() >= kIdMask, die("term id"));
    IF(!slot.empty() && (rec.size() + 1) * 2 <= slot.size(), return);
    rehash(slot.empty() ? kCap : slot.size() * 2);
  }

  void insert_slot(u32 id) {
    u64 h = mix_tok(rec[id].w0, rec[id].w1, rec[id].n);
    u32 tag = tag_of(h);
    FB(size_t i = h & mask;; i = (i + 1) & mask, IB(!slot[i], slot[i] = pack(tag, id); return));
  }

  bool eq_at(u32 id, u64 w0, u64 w1, u32 n, const uint8_t* s) const {
    const Rec& r = rec[id];
    IF(r.n != n || r.w0 != w0 || (n > 8 && r.w1 != w1), return false);
    return n <= 16 || tail_eq<false>((const uint8_t*)text.data() + r.off, s, n);
  }

  u32 add(const uint8_t* s, u32 n) {
    grow();
    u64 w0, w1;
    words_of(s, n, w0, w1);
    u64 h = mix_tok(w0, w1, n);
    u32 tag = tag_of(h);
    FB(size_t i = h & mask;; i = (i + 1) & mask, u32 sl = slot[i];
       IB(!sl, Rec r; r.w0 = w0; r.w1 = w1; r.n = (uint8_t)n; r.off = (u32)text.size();
          text.resize(text.size() + n);
          FOR(u32 k = 0; k < n; ++k, text[r.off + k] = (char)(s[k] | 0x20u)); rec.push_back(r);
          slot[i] = pack(tag, (u32)rec.size() - 1); return (u32)rec.size() - 1);
       IF((sl >> kIdBits) == tag && eq_at((sl & kIdMask) - 1u, w0, w1, n, s), return (sl & kIdMask) - 1u));
  }

  void renumber(const vector<u32>& neu) {
    vector<Rec> nr(rec.size());
    FOR(u32 i = 0; i < rec.size(); ++i, nr[neu[i]] = rec[i]);
    rec.swap(nr);
    rehash(max(slot.size(), kCap));
  }

  void prefetch(u64 h) const { _mm_prefetch((const char*)&slot[h & mask], _MM_HINT_T0); }

  u32 find_fast(u64 w0, u64 w1, u32 n, u64 h, const uint8_t* s) const {
    u32 tag = tag_of(h);
    FB(size_t i = h & mask;; i = (i + 1) & mask, u32 sl = slot[i]; IF(!sl, return kNone);
       IF((sl >> kIdBits) == tag && eq_at((sl & kIdMask) - 1u, w0, w1, n, s), return (sl & kIdMask) - 1u));
  }
};

struct DocSeen {
  struct E {
    u64 w0 = 0, w1 = 0;
    const uint8_t* s = nullptr;
    u32 n = 0, gen = 0;
  };
  static constexpr u32 kM = 4096;
  E tab[kM];
  u32 gen = 1;
  void next() { IB(++gen == 0, memset(tab, 0, sizeof tab); gen = 1); }

  bool claim(u64 h, u64 w0, u64 w1, u32 n, const uint8_t* s) {
    u32 i = (u32)h & (kM - 1);
    FB(u32 k = 0; k < kM; ++k, E& e = tab[i];
       IB(e.gen != gen, e.w0 = w0; e.w1 = w1; e.n = n; e.s = s; e.gen = gen; return true);
       IF(e.w0 == w0 && e.n == n && (n <= 8 || e.w1 == w1) && (n <= 16 || tail_eq<true>(e.s, s, n)), return false);
       i = (i + 1) & (kM - 1));
    die("doc table");
  }
};

inline u32 token_mask32(const uint8_t* p) {
  __m256i v = _mm256_loadu_si256((const __m256i*)p);
  __m256i low = _mm256_or_si256(v, _mm256_set1_epi8(0x20));
  __m256i letters = _mm256_sub_epi8(low, _mm256_set1_epi8('a'));
  __m256i digits = _mm256_sub_epi8(v, _mm256_set1_epi8('0'));
  __m256i zero = _mm256_setzero_si256();
  __m256i in = _mm256_or_si256(_mm256_cmpeq_epi8(_mm256_subs_epu8(letters, _mm256_set1_epi8(25)), zero),
                              _mm256_cmpeq_epi8(_mm256_subs_epu8(digits, _mm256_set1_epi8(9)), zero));
  return (u32)_mm256_movemask_epi8(in);
}

template <class F>
void for_tokens(const uint8_t* q, const uint8_t* e, const Terms& terms, DocSeen& seen, F&& emit) {
  struct P {
    const uint8_t* s;
    u32 n;
    u64 w0, w1, h;
  };
  P buf[2][kLag];
  int cur = 0, cn = 0, prev = -1, pn = 0;
  auto probe = [&](int g, int n) {
    FB(int i = 0; i < n; ++i, const P& t = buf[g][i]; emit(terms.find_fast(t.w0, t.w1, t.n, t.h, t.s)));
  };
  auto push = [&](const uint8_t* s, u32 len) {
    IF(len == 0 || len > kMaxTerm, return);
    u64 w0, w1;
    words_of(s, len, w0, w1);
    u64 h = mix_tok(w0, w1, len);
    IF(!seen.claim(h, w0, w1, len, s), return);
    IB(cn == kLag, FB(int i = 0; i < cn; ++i, terms.prefetch(buf[cur][i].h)); IF(prev >= 0, probe(prev, pn));
       prev = cur; pn = cn; cur ^= 1; cn = 0);
    buf[cur][cn++] = P{s, len, w0, w1, h};
  };
  const uint8_t* tok = nullptr;
  auto flush = [&](const uint8_t* end) { IB(tok, push(tok, (u32)(end - tok)); tok = nullptr); };
  const uint8_t* p = q;
  WB(p + 32 <= e, u32 m = token_mask32(p); IB(m == 0, flush(p); p += 32; continue);
     IB(m == ~0u, IF(!tok, tok = p); p += 32; continue); u32 bit = 0;
     IB(tok, IB(m & 1u, u32 run = (u32)__builtin_ctz(~m); flush(p + run); bit = run) EL(flush(p)));
     WB(bit < 32, u32 rest = m >> bit; IF(!rest, break); bit += (u32)__builtin_ctz(rest); IF(bit >= 32, break);
        rest = m >> bit; u32 ones = (u32)__builtin_ctz(~rest);
        IB(bit + ones >= 32, tok = p + bit; break); push(p + bit, ones); bit += ones);
     p += 32);
  WB(p < e, IB(is_token(*p), IF(!tok, tok = p)) EL(IF(tok, flush(p))); ++p);
  flush(e);
  IF(cn, FB(int i = 0; i < cn; ++i, terms.prefetch(buf[cur][i].h)));
  IF(prev >= 0, probe(prev, pn));
  IF(cn, probe(cur, cn));
}

struct BlockLog {
  static constexpr size_t kBlock = 1 << 18, kBytes = 3 * kBlock + 4;
  vector<uint8_t*> blocks;
  uint8_t* cur = nullptr;
  size_t fill = kBlock;

  void push(u32 x) {
    IB(fill == kBlock,
       cur = (uint8_t*)mmap(nullptr, kBytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
       IF(cur == MAP_FAILED, die("mmap log")); blocks.push_back(cur); fill = 0);
    memcpy(cur + 3 * fill, &x, 4);
    fill++;
  }

  template <class F>
  void drain(F&& fn) {
    FB(size_t b = 0; b < blocks.size(); ++b, size_t n = b + 1 == blocks.size() ? fill : kBlock;
       FB(size_t i = 0; i < n; ++i, u32 x = 0; memcpy(&x, blocks[b] + 3 * i, 4); fn(x & 0xFFFFFFu));
       munmap(blocks[b], kBytes));
    blocks.clear();
  }

  ~BlockLog() { FOR(uint8_t* b : blocks, munmap(b, kBytes)); }
};

struct Index {
  u32 ndocs = 0, words = 0;
  int threads = 0;
  vector<u32> dense, df, origin, start, text;
  vector<u64> bits;
  vector<vector<u32>> seg_end;
  uint16_t* post = nullptr;
  size_t post_n = 0;
  ~Index() { IF(post, munmap(post, post_n * sizeof(uint16_t))); }
};

u32 parse_id(const uint8_t* p) {
  u32 v = 0;
  WH(*p >= '0' && *p <= '9', v = v * 10u + (u32)(*p++ - '0'));
  return v;
}

template <class F>
u32 walk_docs(const uint8_t* begin, const uint8_t* end, const uint8_t* stop, u32 doc, F&& fn) {
  FB(const uint8_t* p = begin; p < stop && p < end;, const uint8_t* nl = (const uint8_t*)memchr(p, '\n', end - p);
     const uint8_t* tab = (const uint8_t*)memchr(p, '\t', nl - p); fn(doc, tab + 1, nl); p = nl + 1; ++doc);
  return doc;
}

template <class F>
void parallel(int n, F fn) {
  vector<thread> pool;
  FB(int t = 0; t < n; ++t, pool.emplace_back([&, t] { fn(t); }));
  FOR(auto& th : pool, th.join());
}

struct Qry {
  bool phrase = false;
  u32 begin = 0, n = 0, op_at = 0;
};

Index build(const FileMap& file, Terms& terms, vector<u32>& qterms, int T) {
  Index ix;
  const uint8_t* base = file.data;
  const size_t size = file.size;
  IB(size, size_t last = size - 1; WH(last > 0 && base[last - 1] != '\n', --last); ix.ndocs = parse_id(base + last) + 1);
  ix.threads = T;
  ix.words = (ix.ndocs + 63) / 64;
  ix.origin.resize((size_t)T);
  ix.text.assign((size_t)ix.ndocs * 2, 0);
  const u32 nterms = terms.size();
  ix.dense.assign(nterms, kNone);
  const u32 nbuckets = (nterms + kBucket - 1) / kBucket;
  vector<size_t> cut((size_t)T + 1);
  FB(int t = 0; t <= T; ++t, size_t c = size * (size_t)t / (size_t)T; WH(c > 0 && c < size && base[c - 1] != '\n', ++c);
     cut[t] = c);
  vector<vector<u32>> count((size_t)T);
  vector<u32> sampled((size_t)T);
  parallel(T, [&](int t) {
    const uint8_t* begin = base + cut[t];
    const uint8_t* end = base + cut[t + 1];
    u32 first = begin < end ? parse_id(begin) : 0;
    ix.origin[t] = first;
    auto& cnt = count[t];
    cnt.assign(nterms, 0);
    DocSeen doc_seen;
    const uint8_t* stop = begin + (end - begin) / 32;
    u32 doc = walk_docs(begin, end, stop, first, [&](u32, const uint8_t* tx, const uint8_t* nl) {
      doc_seen.next();
      for_tokens(tx, nl, terms, doc_seen, [&](u32 id) { IF(id < nterms, cnt[id]++); });
    });
    sampled[t] = doc - first;
  });
  u64 sample_docs = 0;
  vector<u64> sdf(nterms);
  FB(int t = 0; t < T; ++t, sample_docs += sampled[t]; FOR(u32 id = 0; id < nterms; ++id, sdf[id] += count[t][id]));
  vector<u32> order(nterms), neu(nterms), dense_new(nterms, kNone);
  FOR(u32 id = 0; id < nterms; ++id, order[id] = id);
  stable_sort(order.begin(), order.end(), [&](u32 a, u32 b) { return sdf[a] > sdf[b]; });
  u32 ndense = 0;
  FB(u32 rank = 0; rank < nterms; ++rank, neu[order[rank]] = rank;
     IF(sdf[order[rank]] * 32 > sample_docs, dense_new[rank] = ndense++));
  terms.renumber(neu);
  FOR(u32& id : qterms, id = neu[id]);
  ix.dense.swap(dense_new);
  ix.bits.assign((size_t)ndense * ix.words, 0);
  vector<vector<BlockLog>> logs((size_t)T);
  FB(int t = 0; t < T; ++t, count[t].assign(nterms, 0));
  parallel(T, [&](int t) {
    const uint8_t* begin = base + cut[t];
    const uint8_t* end = base + cut[t + 1];
    u32 first = ix.origin[t];
    auto& cnt = count[t];
    DocSeen doc_seen;
    auto& out = logs[t];
    out.resize(nbuckets);
    u32 doc = walk_docs(begin, end, end, first, [&](u32 doci, const uint8_t* tx, const uint8_t* nl) {
      ix.text[2 * doci] = (u32)(tx - base);
      ix.text[2 * doci + 1] = (u32)(nl - base);
      doc_seen.next();
      for_tokens(tx, nl, terms, doc_seen, [&](u32 id) {
        IF(id >= nterms, return);
        IB(u32 d = ix.dense[id]; d != kNone,
           __atomic_or_fetch(&ix.bits[(size_t)d * ix.words + (doci >> 6)], 1ull << (doci & 63), __ATOMIC_RELAXED))
        EB(out[id >> kTermBits].push(((id & (kBucket - 1)) << kDocBits) | (doci - first)); cnt[id]++);
      });
    });
    IF(doc - first >= (1u << kDocBits), die("doc offset overflow"));
  });
  ix.start.resize(nterms + 1);
  u32 total = 0;
  FB(u32 id = 0; id < nterms; ++id, ix.start[id] = total;
     FB(int t = 0; t < T; ++t, u32 c = count[t][id]; count[t][id] = total; total += c));
  ix.start[nterms] = total;
  ix.post_n = total;
  void* mem = mmap(nullptr, max<size_t>(total, 1) * sizeof(uint16_t), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                   -1, 0);
  IF(mem == MAP_FAILED, die("mmap post"));
  ix.post = (uint16_t*)mem;
  parallel(T, [&](int t) {
    FB(u32 b = 0; b < nbuckets; ++b, logs[t][b].drain([&](u32 x) {
         u32 id = (b << kTermBits) | (x >> kDocBits);
         ix.post[count[t][id]++] = (uint16_t)(x & ((1u << kDocBits) - 1));
       }));
  });
  ix.seg_end = move(count);
  ix.df.assign(nterms, 0);
  FB(u32 id = 0; id < nterms; ++id, IB(ix.dense[id] != kNone, u32 pc = 0;
                                       const u64* b = ix.bits.data() + (size_t)ix.dense[id] * ix.words;
                                       FB(u32 w = 0; w < ix.words; ++w, pc += (u32)__builtin_popcountll(b[w]));
                                       ix.df[id] = pc)
                                  EL(ix.df[id] = ix.start[id + 1] - ix.start[id]));
  return ix;
}

struct Post {
  const u32* p = nullptr;
  size_t n = 0;
  const u64* bits = nullptr;
};

size_t gallop(const u32* a, size_t i, size_t n, u32 x) {
  size_t step = 1, hi = i;
  WB(hi < n && a[hi] < x, i = hi + 1; hi += step; IF(step < (1u << 30), step <<= 1));
  IF(hi > n, hi = n);
  return (size_t)(lower_bound(a + i, a + hi, x) - a);
}

void op_and(const vector<u32>& a, const Post& b, vector<u32>& out) {
  out.clear();
  IF(!b.n, return);
  IB(b.bits, FOR(u32 x : a, IF((b.bits[x >> 6] >> (x & 63)) & 1ull, out.push_back(x))); return);
  IB(a.size() * 16 < b.n, size_t j = 0;
     FB(u32 x : a, j = gallop(b.p, j, b.n, x); IF(j == b.n, break); IF(b.p[j] == x, out.push_back(x))); return);
  IB(b.n * 16 < a.size(), size_t i = 0; FB(size_t k = 0; k < b.n; ++k, i = gallop(a.data(), i, a.size(), b.p[k]);
                                           IF(i == a.size(), break); IF(a[i] == b.p[k], out.push_back(b.p[k])));
     return);
  size_t i = 0, j = 0;
  WB(i < a.size() && j < b.n, IB(a[i] == b.p[j], out.push_back(a[i]); ++i; ++j) EI(a[i] < b.p[j], ++i); EL(++j));
}

void op_merge(const vector<u32>& a, const Post& b, vector<u32>& out, bool keep_b) {
  out.clear();
  IB(!b.n, out = a; return);
  IB(b.bits, FOR(u32 x : a, IF(((b.bits[x >> 6] >> (x & 63)) & 1ull) == 0, out.push_back(x))); return);
  size_t i = 0, j = 0;
  WB(i < a.size() && j < b.n, IB(a[i] == b.p[j], IF(keep_b, out.push_back(a[i])); ++i; ++j)
     EI(a[i] < b.p[j], out.push_back(a[i++])); EB(IF(keep_b, out.push_back(b.p[j])); ++j));
  WH(i < a.size(), out.push_back(a[i++]));
  IF(keep_b, WH(j < b.n, out.push_back(b.p[j++])));
}

u64 bit_index_sum(u64 m) {
  static constexpr u64 kSel[6] = {0xAAAAAAAAAAAAAAAAull, 0xCCCCCCCCCCCCCCCCull, 0xF0F0F0F0F0F0F0F0ull,
                                  0xFF00FF00FF00FF00ull, 0xFFFF0000FFFF0000ull, 0xFFFFFFFF00000000ull};
  u64 s = 0;
  FB(int k = 0; k < 6; ++k, s += (u64)__builtin_popcountll(m & kSel[k]) << k);
  return s;
}

struct Engine {
  const Index* ix = nullptr;
  const Terms* terms = nullptr;
  const FileMap* docs = nullptr;
  const vector<u32>* qterms = nullptr;
  const vector<char>* qops = nullptr;
  vector<u32> cur, tmp, list, order;
  vector<u64> bm;
  bool dense = false;

  Post decode(u32 id) {
    Post r;
    IB(u32 d = ix->dense[id]; d != kNone, r.bits = ix->bits.data() + (size_t)d * ix->words; r.n = ix->df[id]; return r);
    list.resize(ix->df[id]);
    u32 i = ix->start[id];
    size_t n = 0;
    FB(int t = 0; t < ix->threads; ++t, u32 e = ix->seg_end[t][id], org = ix->origin[t];
       WH(i < e, list[n++] = org + ix->post[i++]));
    r.p = list.data(), r.n = n;
    return r;
  }

  void boolean(u32 begin, u32 n, u32 op_at) {
    Post first = decode((*qterms)[begin]);
    dense = first.bits != nullptr;
    IF(dense, bm.assign(first.bits, first.bits + ix->words));
    EL(cur.assign(first.p, first.p + first.n));
    FB(u32 k = 1; k < n; ++k, char op = (*qops)[op_at + (k - 1)]; Post b = decode((*qterms)[begin + k]);
       IB(dense, apply_dense(op, b))
       EIB(b.bits && op == 'O', bm.assign(b.bits, b.bits + ix->words);
           FOR(u32 x : cur, bm[x >> 6] |= 1ull << (x & 63)); dense = true)
       EB(IF(op == 'A', op_and(cur, b, tmp)); EL(op_merge(cur, b, tmp, op == 'O')); cur.swap(tmp)));
  }

  void apply_dense(char op, const Post& b) {
    IB(b.bits, FB(u32 w = 0; w < ix->words; ++w, u64 x = b.bits[w];
                  bm[w] = op == 'A' ? bm[w] & x : op == 'O' ? bm[w] | x : bm[w] & ~x);
       return);
    IB(op == 'A', cur.clear();
       FB(size_t i = 0; i < b.n; ++i, IF((bm[b.p[i] >> 6] >> (b.p[i] & 63)) & 1ull, cur.push_back(b.p[i])));
       dense = false; return);
    FB(size_t i = 0; i < b.n; ++i, u32 id = b.p[i]; IF(op == 'O', bm[id >> 6] |= 1ull << (id & 63));
       EL(bm[id >> 6] &= ~(1ull << (id & 63))));
  }

  bool token_is(const uint8_t* p, size_t n, u32 id) const {
    IF(n != terms->rec[id].n, return false);
    const uint8_t* k = terms->bytes(id);
    FOR(size_t i = 0; i < n; ++i, IF((p[i] | 0x20) != k[i], return false));
    return true;
  }

  bool around(const uint8_t* b, const uint8_t* e, const uint8_t* s, const uint8_t* t, const u32* phrase, u32 pn,
              u32 j) const {
    FB(u32 i = j; i-- > 0;, WH(s > b && !is_token(s[-1]), --s); const uint8_t* end = s;
       WH(s > b && is_token(s[-1]), --s); IF(s == end || !token_is(s, (size_t)(end - s), phrase[i]), return false));
    FB(u32 i = j + 1; i < pn; ++i, WH(t < e && !is_token(*t), ++t); const uint8_t* st = t;
       WH(t < e && is_token(*t), ++t); IF(t == st || !token_is(st, (size_t)(t - st), phrase[i]), return false));
    return true;
  }

  bool contains(const uint8_t* b, const uint8_t* e, const u32* phrase, u32 pn, u32 j) const {
    u32 anchor = phrase[j];
    uint8_t first = terms->bytes(anchor)[0];
    const __m256i want = _mm256_set1_epi8((char)first), fold = _mm256_set1_epi8(0x20);
    FB(const uint8_t* p = b; p < e;, u32 m = 0; int room = (int)(e - p), step = room >= 32 ? 32 : room;
       IB(room >= 32, __m256i v = _mm256_loadu_si256((const __m256i*)p);
          m = (u32)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_or_si256(v, fold), want)))
       EL(FB(int i = 0; i < room; ++i, m |= (u32)(((p[i] | 0x20) == first) << i)));
       WB(m, const uint8_t* s = p + __builtin_ctz(m); m &= m - 1; IF(s > b && is_token(s[-1]), continue);
          const uint8_t* t = s; WH(t < e && is_token(*t), ++t);
          IF(token_is(s, (size_t)(t - s), anchor) && around(b, e, s, t, phrase, pn, j), return true));
       p += step);
    return false;
  }

  void phrase(u32 begin, u32 n) {
    dense = false;
    order.clear();
    const u32* ph = qterms->data() + begin;
    FB(u32 i = 0; i < n; ++i, order.push_back(ph[i]));
    sort(order.begin(), order.end());
    order.erase(unique(order.begin(), order.end()), order.end());
    sort(order.begin(), order.end(), [&](u32 a, u32 b) { return ix->df[a] < ix->df[b]; });
    IB(order.empty() || ix->df[order[0]] == 0, cur.clear(); return);
    Post a = decode(order[0]);
    IB(a.bits, cur.clear(); FB(u32 w = 0; w < ix->words; ++w, u64 x = a.bits[w];
                               WB(x, u32 b = (u32)__builtin_ctzll(x); u32 id = w * 64u + b; IF(id >= ix->ndocs, break);
                                  cur.push_back(id); x &= x - 1)))
    EL(cur.assign(a.p, a.p + a.n));
    FB(size_t i = 1; i < order.size() && !cur.empty(); ++i, op_and(cur, decode(order[i]), tmp); cur.swap(tmp));
    u32 j = 0;
    WH(j < n && ph[j] != order[0], j++);
    size_t kept = 0;
    FB(u32 doc : cur, const uint8_t* b = docs->data + ix->text[2 * doc];
       const uint8_t* e = docs->data + ix->text[2 * doc + 1]; IF(contains(b, e, ph, n, j), cur[kept++] = doc));
    cur.resize(kept);
  }

  int format(char* out) const {
    u64 count = 0, sum = 0;
    u32 small[10];
    size_t m = 0;
    IB(dense, FB(u32 w = 0; w < ix->words; ++w, u64 bits = bm[w]; u64 c = (u64)__builtin_popcountll(bits);
                 count += c; sum += c * 64ull * w + bit_index_sum(bits);
                 WB(bits && m < 10, u32 id = w * 64u + (u32)__builtin_ctzll(bits); IF(id >= ix->ndocs, break);
                    small[m++] = id; bits &= bits - 1)))
    EB(count = cur.size(); FOR(u32 x : cur, sum += x); WH(m < count && m < 10, small[m] = cur[m], ++m));
    auto put = [](char* p, u64 v) {
      char tmp[24];
      int n = 0;
      do tmp[n++] = (char)('0' + v % 10), v /= 10;
      while (v);
      WH(n, *p++ = tmp[--n]);
      return p;
    };
    char* p = put(out, count);
    *p++ = ' ';
    p = put(p, sum);
    FB(size_t i = 0; i < m; ++i, *p++ = ' '; p = put(p, small[i]));
    *p++ = '\n';
    return (int)(p - out);
  }
};

}  // namespace

int main() {
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
  mallopt(M_MMAP_THRESHOLD, 1 << 16);
  mallopt(M_TRIM_THRESHOLD, 1 << 16);
  mallopt(M_TOP_PAD, 0);
  std::string dir = env_or("DATA", "/data");
  std::string out_path = std::string(env_or("OUT", "/out")) + "/results.txt";
  FileMap qf((dir + "/queries.txt").c_str()), df((dir + "/docs.txt").c_str());
  Terms terms;
  std::vector<Qry> queries;
  std::vector<u32> qterms;
  std::vector<char> qops;
  queries.reserve(50000);
  static constexpr char kOp[] = {'A', 'O', 'N'};
  static constexpr int kOpN[] = {3, 2, 3};
  static constexpr char kOpW[][4] = {"AND", "OR", "NOT"};
  const uint8_t* p = qf.data;
  const uint8_t* qe = p + qf.size;
  WB(p < qe, const uint8_t* line = p; WH(p < qe && *p != '\n', ++p); const uint8_t* lend = p; IF(p < qe, ++p); Qry q;
     IB((size_t)(lend - line) >= 5 && memcmp(line, "BOOL ", 5) == 0, q.begin = (u32)qterms.size();
        q.op_at = (u32)qops.size(); const uint8_t* s = line + 5;
        WB(s < lend, const uint8_t* t = s; WH(s < lend && *s != ' ', ++s);
           qterms.push_back(terms.add(t, (u32)(s - t))); IF(s >= lend, break); ++s;
           FB(int k = 0; k < 3; ++k,
              IB(s + kOpN[k] <= lend && memcmp(s, kOpW[k], kOpN[k]) == 0, qops.push_back(kOp[k]); s += kOpN[k]; break));
           IF(s < lend && *s == ' ', ++s));
        q.n = (u32)(qterms.size() - q.begin))
     EIB((size_t)(lend - line) >= 7 && memcmp(line, "PHRASE ", 7) == 0, q.phrase = true;
        q.begin = (u32)qterms.size(); const uint8_t* s = line + 7; IF(s < lend && *s == '"', ++s);
        WB(s < lend && *s != '"', const uint8_t* t = s; WH(s < lend && *s != ' ' && *s != '"', ++s);
           qterms.push_back(terms.add(t, (u32)(s - t))); IF(s < lend && *s == ' ', ++s));
        q.n = (u32)(qterms.size() - q.begin));
     queries.push_back(q));
  int T = thread_count();
  Index ix = build(df, terms, qterms, T);
  malloc_trim(0);
  malloc_trim(0);
  constexpr int kSlot = 128;
  const size_t nq = queries.size();
  std::vector<char> slots(nq * kSlot);
  std::vector<int> lens(nq);
  std::atomic<size_t> next{0};
  parallel(T, [&](int) {
    Engine eng;
    eng.ix = &ix;
    eng.terms = &terms;
    eng.docs = &df;
    eng.qterms = &qterms;
    eng.qops = &qops;
    WB(true, size_t i = next.fetch_add(1, std::memory_order_relaxed); IF(i >= nq, break); const Qry& q = queries[i];
       IF(q.phrase, eng.phrase(q.begin, q.n)); EL(eng.boolean(q.begin, q.n, q.op_at));
       lens[i] = eng.format(slots.data() + i * kSlot));
  });
  int fd = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  IF(fd < 0, die("open output"));
  FB(size_t i = 0; i < nq; ++i, const char* s = slots.data() + i * kSlot; size_t n = (size_t)lens[i];
     WB(n, ssize_t k = ::write(fd, s, n); IF(k < 0, die("write")); s += k; n -= (size_t)k));
  ::close(fd);
  return 0;
}
