// Track B. Index only terms that appear in the queries.
// Frequent terms are bitmaps. The rest are 16-bit document offsets, filled
// from a 3-byte scan log that is dropped bucket by bucket. Phrases are
// checked against the raw document text. BOOL is strictly left to right,
// and a bitmap result stays a bitmap.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <malloc.h>
#include <string>
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
constexpr int kMaxTermLen = 32;
constexpr int kPrefetchGroup = 8;
constexpr u32 kIdBits = 18;
constexpr u32 kIdMask = (1u << kIdBits) - 1u;
constexpr u32 kTermBits = 8;
constexpr u32 kDocBits = 16;
constexpr u32 kBucket = 1u << kTermBits;
constexpr u64 kAsciiFold = 0x2020202020202020ull;

[[noreturn]] void die(const char *message) {
  fprintf(stderr, "track-b: %s\n", message);
  exit(1);
}

int worker_count() {
  if (const char *env = getenv("SD_THREADS")) {
    int n = atoi(env);
    if (n >= 1 && n <= 32)
      return n;
  }
  unsigned cores = thread::hardware_concurrency();
  if (cores == 0)
    cores = 4;
  return (int)(min(12u, max(8u, cores)));
}

bool is_token(unsigned char c) {
  unsigned char folded = (unsigned char)(c | 0x20);
  return (folded >= 'a' && folded <= 'z') || (c >= '0' && c <= '9');
}

const char *env_or(const char *key, const char *fallback) {
  const char *value = getenv(key);
  return value && *value ? value : fallback;
}

bool doc_bit(const u64 *words, u32 doc) {
  return (words[doc >> 6] >> (doc & 63)) & 1ull;
}

struct FileMap {
  const uint8_t *data = nullptr;
  size_t size = 0;

  explicit FileMap(const char *path) {
    int fd = ::open(path, O_RDONLY);
    if (fd < 0)
      die("open");
    struct stat st {};
    if (fstat(fd, &st) != 0)
      die("fstat");
    size = (size_t)(st.st_size);
    void *reserve = mmap(nullptr, size + 4096, PROT_READ,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (reserve == MAP_FAILED)
      die("mmap");
    if (size && mmap(reserve, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd,
                     0) == MAP_FAILED)
      die("mmap file");
    data = (const uint8_t *)(reserve);
    ::close(fd);
  }
};

// One or two word loads. Bytes past n are masked off. The file map has a
// guard page, so the loads stay in range.
inline void words_of(const uint8_t *text, u32 n, u64 &word0, u64 &word1) {
  u64 first = 0;
  memcpy(&first, text, 8);
  first |= kAsciiFold;
  if (n <= 8) {
    if (n < 8)
      first &= (1ull << (8 * n)) - 1;
    word0 = first;
    word1 = 0;
    return;
  }
  u64 second = 0;
  memcpy(&second, text + 8, 8);
  second |= kAsciiFold;
  if (n < 16)
    second &= (1ull << (8 * (n - 8))) - 1;
  word0 = first;
  word1 = second;
}

inline u64 mix_token(u64 word0, u64 word1, u32 n) {
  u64 hash = (word0 ^ (word1 + ((u64)(n) << 1))) * 0xC2B2AE3D27D4EB4Full;
  return hash ^ (hash >> 33);
}

template <bool Raw>
inline bool tail_eq(const uint8_t *stored, const uint8_t *text, u32 n) {
  for (u32 i = 16; i < n; i++) {
    unsigned char left = Raw ? stored[i] | 0x20 : stored[i];
    if (left != (text[i] | 0x20))
      return false;
  }
  return true;
}

struct TermRec {
  u64 word0 = 0, word1 = 0;
  u32 text_off = 0;
  uint8_t length = 0;
};

// 32-bit slots, sized for L2. Low 18 bits are id+1, high 14 are a tag.
struct Terms {
  static constexpr size_t kCap = 1u << 18;
  vector<u32> slot;
  vector<TermRec> rec;
  vector<char> text;
  size_t mask = 0;

  u32 size() const { return (u32)(rec.size()); }
  const uint8_t *bytes(u32 id) const {
    return (const uint8_t *)(text.data() + rec[id].text_off);
  }
  static u32 tag_of(u64 hash) { return (u32)(hash >> 50); }
  static u32 pack(u32 tag, u32 id) { return (tag << kIdBits) | (id + 1u); }

  void rehash(size_t cap) {
    slot.assign(cap, 0);
    mask = cap - 1;
    for (u32 id = 0; id < rec.size(); id++)
      insert_slot(id);
  }

  void grow() {
    if (rec.size() >= kIdMask)
      die("term id");
    if (!slot.empty() && (rec.size() + 1) * 2 <= slot.size())
      return;
    rehash(slot.empty() ? kCap : slot.size() * 2);
  }

  void insert_slot(u32 id) {
    u64 hash = mix_token(rec[id].word0, rec[id].word1, rec[id].length);
    u32 tag = tag_of(hash);
    for (size_t i = hash & mask;; i = (i + 1) & mask) {
      if (!slot[i]) {
        slot[i] = pack(tag, id);
        return;
      }
    }
  }

  bool eq_at(u32 id, u64 word0, u64 word1, u32 n,
             const uint8_t *text_in) const {
    const TermRec &term = rec[id];
    if (term.length != n || term.word0 != word0 ||
        (n > 8 && term.word1 != word1))
      return false;
    return n <= 16 || tail_eq<false>(bytes(id), text_in, n);
  }

  u32 add(const uint8_t *text_in, u32 n) {
    grow();
    u64 word0, word1;
    words_of(text_in, n, word0, word1);
    u64 hash = mix_token(word0, word1, n);
    u32 tag = tag_of(hash);
    for (size_t i = hash & mask;; i = (i + 1) & mask) {
      u32 packed = slot[i];
      if (!packed) {
        TermRec term;
        term.word0 = word0;
        term.word1 = word1;
        term.length = (uint8_t)(n);
        term.text_off = (u32)(text.size());
        text.resize(text.size() + n);
        for (u32 k = 0; k < n; k++)
          text[term.text_off + k] = (char)(text_in[k] | 0x20);
        rec.push_back(term);
        u32 id = (u32)(rec.size() - 1);
        slot[i] = pack(tag, id);
        return id;
      }
      if ((packed >> kIdBits) == tag &&
          eq_at((packed & kIdMask) - 1u, word0, word1, n, text_in))
        return (packed & kIdMask) - 1u;
    }
  }

  void renumber(const vector<u32> &neu) {
    vector<TermRec> ordered(rec.size());
    for (u32 i = 0; i < rec.size(); i++)
      ordered[neu[i]] = rec[i];
    rec.swap(ordered);
    rehash(max(slot.size(), kCap));
  }

  void prefetch(u64 hash) const {
    _mm_prefetch((const char *)(&slot[hash & mask]), _MM_HINT_T0);
  }

  // Home slot first. The table stays under half full, so this returns
  // immediately for a miss and for most hits.
  u32 find(u64 word0, u64 word1, u32 n, u64 hash,
           const uint8_t *text_in) const {
    u32 tag = tag_of(hash);
    for (size_t i = hash & mask;; i = (i + 1) & mask) {
      u32 packed = slot[i];
      if (!packed)
        return kNone;
      if ((packed >> kIdBits) == tag &&
          eq_at((packed & kIdMask) - 1u, word0, word1, n, text_in))
        return (packed & kIdMask) - 1u;
    }
  }
};

// Distinct tokens in the current document. A repeat skips the term-table probe.
struct DocSeen {
  struct Entry {
    u64 word0 = 0, word1 = 0;
    const uint8_t *text = nullptr;
    u32 n = 0, gen = 0;
  };
  static constexpr u32 kSlots = 4096;
  Entry tab[kSlots];
  u32 gen = 1;

  void next() {
    if (++gen == 0) {
      memset(tab, 0, sizeof tab);
      gen = 1;
    }
  }

  bool claim(u64 hash, u64 word0, u64 word1, u32 n, const uint8_t *text) {
    u32 i = (u32)(hash) & (kSlots - 1);
    for (u32 k = 0; k < kSlots; k++) {
      Entry &entry = tab[i];
      if (entry.gen != gen) {
        entry.word0 = word0;
        entry.word1 = word1;
        entry.n = n;
        entry.text = text;
        entry.gen = gen;
        return true;
      }
      if (entry.word0 == word0 && entry.n == n &&
          (n <= 8 || entry.word1 == word1) &&
          (n <= 16 || tail_eq<true>(entry.text, text, n)))
        return false;
      i = (i + 1) & (kSlots - 1);
    }
    die("doc table");
  }
};

inline u32 token_mask32(const uint8_t *p) {
  __m256i bytes = _mm256_loadu_si256((const __m256i *)(p));
  __m256i low = _mm256_or_si256(bytes, _mm256_set1_epi8(0x20));
  __m256i letters = _mm256_sub_epi8(low, _mm256_set1_epi8('a'));
  __m256i digits = _mm256_sub_epi8(bytes, _mm256_set1_epi8('0'));
  __m256i zero = _mm256_setzero_si256();
  __m256i in = _mm256_or_si256(
      _mm256_cmpeq_epi8(_mm256_subs_epu8(letters, _mm256_set1_epi8(25)), zero),
      _mm256_cmpeq_epi8(_mm256_subs_epu8(digits, _mm256_set1_epi8(9)), zero));
  return (u32)(_mm256_movemask_epi8(in));
}

// Emit each distinct token. A group of eight is prefetched, then probed
// while the next group is tokenized. Token starts are mask bits whose
// previous bit is clear. A token that runs off a 32-byte window finishes
// in a later window.
template <class F>
void for_tokens(const uint8_t *begin, const uint8_t *end, const Terms &terms,
                DocSeen &seen, F &&emit) {
  struct Pending {
    const uint8_t *text;
    u32 n;
    u64 word0, word1, hash;
  };
  Pending buf[2][kPrefetchGroup];
  int cur = 0, count = 0, prev = -1, prev_count = 0;

  auto probe = [&](int group, int n) {
    for (int i = 0; i < n; i++) {
      const Pending &token = buf[group][i];
      emit(terms.find(token.word0, token.word1, token.n, token.hash,
                      token.text));
    }
  };
  auto push = [&](const uint8_t *text, u32 len) {
    if (len == 0 || len > kMaxTermLen)
      return;
    u64 word0, word1;
    words_of(text, len, word0, word1);
    u64 hash = mix_token(word0, word1, len);
    if (!seen.claim(hash, word0, word1, len, text))
      return;
    if (count == kPrefetchGroup) {
      for (int i = 0; i < count; i++)
        terms.prefetch(buf[cur][i].hash);
      if (prev >= 0)
        probe(prev, prev_count);
      prev = cur;
      prev_count = count;
      cur ^= 1;
      count = 0;
    }
    buf[cur][count++] = Pending{text, len, word0, word1, hash};
  };
  const uint8_t *open = nullptr;
  auto flush = [&](const uint8_t *at) {
    if (!open)
      return;
    push(open, (u32)(at - open));
    open = nullptr;
  };

  const uint8_t *p = begin;
  while (p + 32 <= end) {
    u32 mask = token_mask32(p);
    if (mask == ~0u) {
      if (!open)
        open = p;
      p += 32;
      continue;
    }
    if (open) {
      if (!mask) {
        flush(p);
      } else {
        u32 gap = (u32)(__builtin_ctz(~mask));
        flush(p + gap);
        mask &= ~((1u << gap) - 1);
      }
    }
    u32 starts = mask & ~(mask << 1);
    while (starts) {
      u32 start = (u32)(__builtin_ctz(starts));
      u32 len = (u32)(__builtin_ctz(~(mask >> start)));
      if (start + len >= 32) {
        open = p + start;
        break;
      }
      push(p + start, len);
      starts &= starts - 1;
    }
    p += 32;
  }
  while (p < end) {
    if (is_token(*p)) {
      if (!open)
        open = p;
    } else if (open) {
      flush(p);
    }
    ++p;
  }
  flush(end);
  if (count)
    for (int i = 0; i < count; i++)
      terms.prefetch(buf[cur][i].hash);
  if (prev >= 0)
    probe(prev, prev_count);
  if (count)
    probe(cur, count);
}

struct BlockLog {
  static constexpr size_t kBlock = 1 << 18;
  static constexpr size_t kBytes = 3 * kBlock + 4;
  vector<uint8_t *> blocks;
  uint8_t *cur = nullptr;
  size_t fill = kBlock;

  void push(u32 value) {
    if (fill == kBlock) {
      cur = (uint8_t *)(mmap(nullptr, kBytes, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
      if (cur == MAP_FAILED)
        die("mmap log");
      blocks.push_back(cur);
      fill = 0;
    }
    memcpy(cur + 3 * fill, &value, 4);
    fill++;
  }

  template <class F> void drain(F &&fn) {
    for (size_t b = 0; b < blocks.size(); b++) {
      size_t n = b + 1 == blocks.size() ? fill : kBlock;
      for (size_t i = 0; i < n; i++) {
        u32 value = 0;
        memcpy(&value, blocks[b] + 3 * i, 4);
        fn(value & 0xFFFFFFu);
      }
      munmap(blocks[b], kBytes);
    }
    blocks.clear();
  }

  ~BlockLog() {
    for (uint8_t *block : blocks)
      munmap(block, kBytes);
  }
};

struct Index {
  u32 ndocs = 0;
  u32 words = 0;
  int threads = 0;
  vector<u32> dense;
  vector<u32> df;
  vector<u32> origin;
  vector<u32> start;
  vector<u32> text;
  vector<u64> bits;
  vector<vector<u32>> seg_end;
  uint16_t *post = nullptr;
  size_t post_n = 0;

  ~Index() {
    if (post)
      munmap(post, post_n * sizeof(uint16_t));
  }
};

u32 parse_id(const uint8_t *p) {
  u32 value = 0;
  while (*p >= '0' && *p <= '9')
    value = value * 10u + (u32)(*p++ - '0');
  return value;
}

template <class F>
u32 walk_docs(const uint8_t *begin, const uint8_t *end, const uint8_t *stop,
              u32 doc, F &&fn) {
  for (const uint8_t *p = begin; p < stop && p < end;) {
    const uint8_t *nl = (const uint8_t *)(memchr(p, '\n', end - p));
    const uint8_t *tab = (const uint8_t *)(memchr(p, '\t', nl - p));
    fn(doc, tab + 1, nl);
    p = nl + 1;
    ++doc;
  }
  return doc;
}

template <class F> void parallel(int n, F fn) {
  vector<thread> pool;
  pool.reserve((size_t)(n));
  for (int t = 0; t < n; t++)
    pool.emplace_back([&, t] { fn(t); });
  for (auto &worker : pool)
    worker.join();
}

struct Query {
  bool phrase = false;
  u32 begin = 0;
  u32 n = 0;
  u32 op_at = 0;
};

// Move interior cuts forward to a multiple of 64 document ids. Each bitmap
// word then has one writer, so the scan can use a plain or.
void align_cuts(const uint8_t *base, vector<size_t> &cut) {
  int threads = (int)(cut.size()) - 1;
  for (int t = 1; t < threads; t++) {
    const uint8_t *p = base + cut[t];
    const uint8_t *lim = base + cut[t + 1];
    u32 id = p < lim ? parse_id(p) : 0;
    u32 want = (id + 63u) & ~63u;
    while (p < lim && id < want) {
      const uint8_t *nl = (const uint8_t *)(memchr(p, '\n', lim - p));
      if (!nl)
        break;
      p = nl + 1;
      ++id;
    }
    cut[t] = (size_t)(p - base);
  }
}

Index build(const FileMap &file, Terms &terms, vector<u32> &query_terms,
            int threads) {
  Index index;
  const uint8_t *base = file.data;
  const size_t size = file.size;
  if (size) {
    size_t last = size - 1;
    while (last > 0 && base[last - 1] != '\n')
      --last;
    index.ndocs = parse_id(base + last) + 1;
  }
  index.threads = threads;
  index.words = (index.ndocs + 63) / 64;
  index.origin.resize((size_t)(threads));
  index.text.assign((size_t)(index.ndocs) * 2, 0);
  const u32 nterms = terms.size();
  index.dense.assign(nterms, kNone);
  const u32 nbuckets = (nterms + kBucket - 1) / kBucket;

  vector<size_t> cut((size_t)(threads) + 1);
  for (int t = 0; t <= threads; t++) {
    size_t at = size * (size_t)(t) / (size_t)(threads);
    while (at > 0 && at < size && base[at - 1] != '\n')
      ++at;
    cut[t] = at;
  }
  align_cuts(base, cut);

  vector<vector<u32>> count((size_t)(threads));
  vector<u32> sampled((size_t)(threads));
  parallel(threads, [&](int t) {
    const uint8_t *begin = base + cut[t];
    const uint8_t *end = base + cut[t + 1];
    u32 first = begin < end ? parse_id(begin) : 0;
    index.origin[t] = first;
    auto &cnt = count[t];
    cnt.assign(nterms, 0);
    DocSeen seen;
    const uint8_t *stop = begin + (end - begin) / 32;
    u32 doc = walk_docs(begin, end, stop, first,
                        [&](u32, const uint8_t *text, const uint8_t *nl) {
                          seen.next();
                          for_tokens(text, nl, terms, seen, [&](u32 id) {
                            if (id < nterms)
                              cnt[id]++;
                          });
                        });
    sampled[t] = doc - first;
  });

  u64 sample_docs = 0;
  vector<u64> sample_df(nterms);
  for (int t = 0; t < threads; t++) {
    sample_docs += sampled[t];
    for (u32 id = 0; id < nterms; id++)
      sample_df[id] += count[t][id];
  }
  vector<u32> order(nterms), neu(nterms), dense_of(nterms, kNone);
  for (u32 id = 0; id < nterms; id++)
    order[id] = id;
  stable_sort(order.begin(), order.end(),
              [&](u32 a, u32 b) { return sample_df[a] > sample_df[b]; });
  u32 ndense = 0;
  for (u32 rank = 0; rank < nterms; rank++) {
    neu[order[rank]] = rank;
    if (sample_df[order[rank]] * 32 > sample_docs)
      dense_of[rank] = ndense++;
  }
  terms.renumber(neu);
  for (u32 &id : query_terms)
    id = neu[id];
  index.dense.swap(dense_of);
  index.bits.assign((size_t)(ndense)*index.words, 0);

  vector<vector<BlockLog>> logs((size_t)(threads));
  for (int t = 0; t < threads; t++)
    count[t].assign(nterms, 0);
  parallel(threads, [&](int t) {
    const uint8_t *begin = base + cut[t];
    const uint8_t *end = base + cut[t + 1];
    u32 first = index.origin[t];
    auto &cnt = count[t];
    DocSeen seen;
    auto &out = logs[t];
    out.resize(nbuckets);
    u32 doc = walk_docs(
        begin, end, end, first,
        [&](u32 doc_id, const uint8_t *text, const uint8_t *nl) {
          index.text[2 * doc_id] = (u32)(text - base);
          index.text[2 * doc_id + 1] = (u32)(nl - base);
          seen.next();
          for_tokens(text, nl, terms, seen, [&](u32 id) {
            if (id >= nterms)
              return;
            if (u32 dense = index.dense[id]; dense != kNone) {
              index.bits[(size_t)(dense)*index.words + (doc_id >> 6)] |=
                  1ull << (doc_id & 63);
            } else {
              out[id >> kTermBits].push(((id & (kBucket - 1)) << kDocBits) |
                                        (doc_id - first));
              cnt[id]++;
            }
          });
        });
    if (doc - first >= (1u << kDocBits))
      die("doc offset overflow");
  });

  index.start.resize(nterms + 1);
  u32 total = 0;
  for (u32 id = 0; id < nterms; id++) {
    index.start[id] = total;
    for (int t = 0; t < threads; t++) {
      u32 n = count[t][id];
      count[t][id] = total;
      total += n;
    }
  }
  index.start[nterms] = total;
  index.post_n = total;
  void *mem = mmap(nullptr, max<size_t>(total, 1) * sizeof(uint16_t),
                   PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mem == MAP_FAILED)
    die("mmap post");
  index.post = (uint16_t *)(mem);
  parallel(threads, [&](int t) {
    for (u32 bucket = 0; bucket < nbuckets; bucket++) {
      logs[t][bucket].drain([&](u32 packed) {
        u32 id = (bucket << kTermBits) | (packed >> kDocBits);
        index.post[count[t][id]++] =
            (uint16_t)(packed & ((1u << kDocBits) - 1));
      });
    }
  });
  index.seg_end = move(count);
  index.df.assign(nterms, 0);
  for (u32 id = 0; id < nterms; id++) {
    if (index.dense[id] == kNone) {
      index.df[id] = index.start[id + 1] - index.start[id];
      continue;
    }
    u32 pop = 0;
    const u64 *words =
        index.bits.data() + (size_t)(index.dense[id]) * index.words;
    for (u32 w = 0; w < index.words; w++)
      pop += (u32)(__builtin_popcountll(words[w]));
    index.df[id] = pop;
  }
  return index;
}

struct Posting {
  const u32 *docs = nullptr;
  size_t n = 0;
  const u64 *bits = nullptr;
};

size_t gallop(const u32 *docs, size_t i, size_t n, u32 target) {
  size_t step = 1, hi = i;
  while (hi < n && docs[hi] < target) {
    i = hi + 1;
    hi += step;
    if (step < (1u << 30))
      step <<= 1;
  }
  if (hi > n)
    hi = n;
  return (size_t)(lower_bound(docs + i, docs + hi, target) - docs);
}

// 'A' keeps the intersection, 'O' the union, 'N' the difference a-b.
// Bitmap OR is applied before this. A bitmap here is AND or NOT.
void merge_docs(const vector<u32> &a, const Posting &b, vector<u32> &out,
                char op) {
  out.clear();
  if (!b.n) {
    if (op != 'A')
      out = a;
    return;
  }
  if (b.bits) {
    for (u32 doc : a)
      if (doc_bit(b.bits, doc) != (op == 'N'))
        out.push_back(doc);
    return;
  }
  if (op == 'A' && a.size() * 16 < b.n) {
    size_t j = 0;
    for (u32 doc : a) {
      j = gallop(b.docs, j, b.n, doc);
      if (j == b.n)
        return;
      if (b.docs[j] == doc)
        out.push_back(doc);
    }
    return;
  }
  if (op == 'A' && b.n * 16 < a.size()) {
    size_t i = 0;
    for (size_t k = 0; k < b.n; k++) {
      i = gallop(a.data(), i, a.size(), b.docs[k]);
      if (i == a.size())
        return;
      if (a[i] == b.docs[k])
        out.push_back(b.docs[k]);
    }
    return;
  }
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.n) {
    if (a[i] == b.docs[j]) {
      if (op != 'N')
        out.push_back(a[i]);
      ++i;
      ++j;
    } else if (a[i] < b.docs[j]) {
      if (op != 'A')
        out.push_back(a[i]);
      ++i;
    } else {
      if (op == 'O')
        out.push_back(b.docs[j]);
      ++j;
    }
  }
  if (op != 'A')
    while (i < a.size())
      out.push_back(a[i++]);
  if (op == 'O')
    while (j < b.n)
      out.push_back(b.docs[j++]);
}

u64 bit_index_sum(u64 mask) {
  static constexpr u64 kSelect[6] = {
      0xAAAAAAAAAAAAAAAAull, 0xCCCCCCCCCCCCCCCCull, 0xF0F0F0F0F0F0F0F0ull,
      0xFF00FF00FF00FF00ull, 0xFFFF0000FFFF0000ull, 0xFFFFFFFF00000000ull};
  u64 sum = 0;
  for (int bit = 0; bit < 6; bit++)
    sum += (u64)(__builtin_popcountll(mask & kSelect[bit])) << bit;
  return sum;
}

struct Engine {
  const Index *index = nullptr;
  const Terms *terms = nullptr;
  const FileMap *docs = nullptr;
  const vector<u32> *query_terms = nullptr;
  const vector<char> *query_ops = nullptr;
  vector<u32> cur, tmp, list, order;
  vector<u64> bitmap;
  bool dense = false;

  Posting decode(u32 id) {
    Posting posting;
    if (u32 slot = index->dense[id]; slot != kNone) {
      posting.bits = index->bits.data() + (size_t)(slot)*index->words;
      posting.n = index->df[id];
      return posting;
    }
    list.resize(index->df[id]);
    u32 at = index->start[id];
    size_t n = 0;
    for (int t = 0; t < index->threads; t++) {
      u32 end = index->seg_end[t][id];
      u32 origin = index->origin[t];
      for (; at < end; at++)
        list[n++] = origin + index->post[at];
    }
    posting.docs = list.data();
    posting.n = n;
    return posting;
  }

  void apply_dense(char op, const Posting &b) {
    if (b.bits) {
      for (u32 w = 0; w < index->words; w++) {
        u64 word = b.bits[w];
        bitmap[w] = op == 'A'   ? bitmap[w] & word
                    : op == 'O' ? bitmap[w] | word
                                : bitmap[w] & ~word;
      }
      return;
    }
    if (op == 'A') {
      cur.clear();
      for (size_t i = 0; i < b.n; i++)
        if (doc_bit(bitmap.data(), b.docs[i]))
          cur.push_back(b.docs[i]);
      dense = false;
      return;
    }
    for (size_t i = 0; i < b.n; i++) {
      u32 doc = b.docs[i];
      if (op == 'O')
        bitmap[doc >> 6] |= 1ull << (doc & 63);
      else
        bitmap[doc >> 6] &= ~(1ull << (doc & 63));
    }
  }

  void boolean(u32 begin, u32 n, u32 op_at) {
    Posting first = decode((*query_terms)[begin]);
    dense = first.bits != nullptr;
    if (dense)
      bitmap.assign(first.bits, first.bits + index->words);
    else
      cur.assign(first.docs, first.docs + first.n);
    for (u32 k = 1; k < n; k++) {
      char op = (*query_ops)[op_at + (k - 1)];
      Posting b = decode((*query_terms)[begin + k]);
      if (dense) {
        apply_dense(op, b);
      } else if (b.bits && op == 'O') {
        bitmap.assign(b.bits, b.bits + index->words);
        for (u32 doc : cur)
          bitmap[doc >> 6] |= 1ull << (doc & 63);
        dense = true;
      } else {
        merge_docs(cur, b, tmp, op);
        cur.swap(tmp);
      }
    }
  }

  bool token_is(const uint8_t *text, size_t n, u32 id) const {
    if (n != terms->rec[id].length)
      return false;
    const uint8_t *key = terms->bytes(id);
    for (size_t i = 0; i < n; i++)
      if ((text[i] | 0x20) != key[i])
        return false;
    return true;
  }

  bool around(const uint8_t *begin, const uint8_t *end, const uint8_t *start,
              const uint8_t *token_end, const u32 *phrase, u32 phrase_n,
              u32 at) const {
    for (u32 i = at; i-- > 0;) {
      while (start > begin && !is_token(start[-1]))
        --start;
      const uint8_t *word_end = start;
      while (start > begin && is_token(start[-1]))
        --start;
      if (start == word_end ||
          !token_is(start, (size_t)(word_end - start), phrase[i]))
        return false;
    }
    for (u32 i = at + 1; i < phrase_n; i++) {
      while (token_end < end && !is_token(*token_end))
        ++token_end;
      const uint8_t *word = token_end;
      while (token_end < end && is_token(*token_end))
        ++token_end;
      if (token_end == word ||
          !token_is(word, (size_t)(token_end - word), phrase[i]))
        return false;
    }
    return true;
  }

  bool contains(const uint8_t *begin, const uint8_t *end, const u32 *phrase,
                u32 phrase_n, u32 at) const {
    u32 anchor = phrase[at];
    const uint8_t *key = terms->bytes(anchor);
    uint8_t first = key[0];
    u32 length = terms->rec[anchor].length;
    // A second byte drops most false hits before the scalar token check.
    // The map has a guard page, so the overlapped load stays in range.
    uint8_t second = length > 1 ? key[1] : 0;
    const __m256i want = _mm256_set1_epi8((char)(first));
    const __m256i want_next = _mm256_set1_epi8((char)(second));
    const __m256i fold = _mm256_set1_epi8(0x20);
    for (const uint8_t *p = begin; p < end;) {
      u32 hits = 0;
      int room = (int)(end - p);
      int step = room >= 32 ? 32 : room;
      if (room >= 32) {
        __m256i bytes = _mm256_loadu_si256((const __m256i *)(p));
        hits = (u32)(_mm256_movemask_epi8(
            _mm256_cmpeq_epi8(_mm256_or_si256(bytes, fold), want)));
        if (length > 1) {
          __m256i next = _mm256_loadu_si256((const __m256i *)(p + 1));
          u32 more = (u32)(_mm256_movemask_epi8(
              _mm256_cmpeq_epi8(_mm256_or_si256(next, fold), want_next)));
          if (room == 32)
            more &= 0x7fffffffu;
          hits &= more;
        }
      } else {
        for (int i = 0; i < room; i++) {
          if ((p[i] | 0x20) != first)
            continue;
          if (length > 1 && (i + 1 >= room || (p[i + 1] | 0x20) != second))
            continue;
          hits |= 1u << i;
        }
      }
      while (hits) {
        const uint8_t *start = p + __builtin_ctz(hits);
        hits &= hits - 1;
        if (start > begin && is_token(start[-1]))
          continue;
        const uint8_t *token_end = start;
        while (token_end < end && is_token(*token_end))
          ++token_end;
        if (token_is(start, (size_t)(token_end - start), anchor) &&
            around(begin, end, start, token_end, phrase, phrase_n, at))
          return true;
      }
      p += step;
    }
    return false;
  }

  void append_bitmap_docs(const u64 *bits) {
    cur.clear();
    for (u32 w = 0; w < index->words; w++) {
      u64 word = bits[w];
      while (word) {
        u32 bit = (u32)(__builtin_ctzll(word));
        u32 doc = w * 64u + bit;
        if (doc >= index->ndocs)
          break;
        cur.push_back(doc);
        word &= word - 1;
      }
    }
  }

  void phrase(u32 begin, u32 n) {
    dense = false;
    order.clear();
    const u32 *words = query_terms->data() + begin;
    for (u32 i = 0; i < n; i++)
      order.push_back(words[i]);
    sort(order.begin(), order.end());
    order.erase(unique(order.begin(), order.end()), order.end());
    sort(order.begin(), order.end(),
         [&](u32 a, u32 b) { return index->df[a] < index->df[b]; });
    if (order.empty() || index->df[order[0]] == 0) {
      cur.clear();
      return;
    }
    Posting rarest = decode(order[0]);
    if (rarest.bits)
      append_bitmap_docs(rarest.bits);
    else
      cur.assign(rarest.docs, rarest.docs + rarest.n);
    for (size_t i = 1; i < order.size() && !cur.empty(); i++) {
      merge_docs(cur, decode(order[i]), tmp, 'A');
      cur.swap(tmp);
    }
    u32 anchor_at = 0;
    while (anchor_at < n && words[anchor_at] != order[0])
      anchor_at++;
    size_t kept = 0;
    for (u32 doc : cur) {
      const uint8_t *text = docs->data + index->text[2 * doc];
      const uint8_t *text_end = docs->data + index->text[2 * doc + 1];
      if (contains(text, text_end, words, n, anchor_at))
        cur[kept++] = doc;
    }
    cur.resize(kept);
  }

  int format(char *out) const {
    u64 count = 0, sum = 0;
    u32 small[10];
    size_t shown = 0;
    if (dense) {
      for (u32 w = 0; w < index->words; w++) {
        u64 bits = bitmap[w];
        u64 n = (u64)(__builtin_popcountll(bits));
        count += n;
        sum += n * 64ull * w + bit_index_sum(bits);
        while (bits && shown < 10) {
          u32 doc = w * 64u + (u32)(__builtin_ctzll(bits));
          if (doc >= index->ndocs)
            break;
          small[shown++] = doc;
          bits &= bits - 1;
        }
      }
    } else {
      count = cur.size();
      for (u32 doc : cur)
        sum += doc;
      while (shown < count && shown < 10) {
        small[shown] = cur[shown];
        shown++;
      }
    }
    auto put = [](char *p, u64 value) {
      char digits[24];
      int n = 0;
      do
        digits[n++] = (char)('0' + value % 10);
      while (value /= 10);
      while (n)
        *p++ = digits[--n];
      return p;
    };
    char *p = put(out, count);
    *p++ = ' ';
    p = put(p, sum);
    for (size_t i = 0; i < shown; i++) {
      *p++ = ' ';
      p = put(p, small[i]);
    }
    *p++ = '\n';
    return (int)(p - out);
  }
};

} // namespace

int main() {
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
  mallopt(M_MMAP_THRESHOLD, 1 << 16);
  mallopt(M_TRIM_THRESHOLD, 1 << 16);
  mallopt(M_TOP_PAD, 0);

  std::string dir = env_or("DATA", "/data");
  std::string out_path = std::string(env_or("OUT", "/out")) + "/results.txt";
  FileMap queries_file((dir + "/queries.txt").c_str());
  FileMap docs_file((dir + "/docs.txt").c_str());

  Terms terms;
  std::vector<Query> queries;
  std::vector<u32> query_terms;
  std::vector<char> query_ops;
  queries.reserve(50000);
  static constexpr char kOpCode[] = {'A', 'O', 'N'};
  static constexpr int kOpLen[] = {3, 2, 3};
  static constexpr char kOpWord[][4] = {"AND", "OR", "NOT"};

  auto take_term = [&](const uint8_t *&cursor, const uint8_t *stop,
                       char extra) {
    const uint8_t *word = cursor;
    while (cursor < stop && *cursor != ' ' && *cursor != extra)
      ++cursor;
    return terms.add(word, (u32)(cursor - word));
  };
  const uint8_t *p = queries_file.data;
  const uint8_t *end = p + queries_file.size;
  while (p < end) {
    const uint8_t *line = p;
    while (p < end && *p != '\n')
      ++p;
    const uint8_t *line_end = p;
    if (p < end)
      ++p;
    Query query;
    const uint8_t *cursor = line;
    bool parsed = false;
    if ((size_t)(line_end - line) >= 5 && memcmp(line, "BOOL ", 5) == 0) {
      parsed = true;
      query.begin = (u32)query_terms.size();
      query.op_at = (u32)query_ops.size();
      cursor = line + 5;
      while (cursor < line_end) {
        query_terms.push_back(take_term(cursor, line_end, 0));
        if (cursor >= line_end)
          break;
        ++cursor;
        for (int k = 0; k < 3; k++) {
          if (cursor + kOpLen[k] <= line_end &&
              memcmp(cursor, kOpWord[k], kOpLen[k]) == 0) {
            query_ops.push_back(kOpCode[k]);
            cursor += kOpLen[k];
            break;
          }
        }
        if (cursor < line_end && *cursor == ' ')
          ++cursor;
      }
    } else if ((size_t)(line_end - line) >= 7 &&
               memcmp(line, "PHRASE ", 7) == 0) {
      parsed = true;
      query.phrase = true;
      query.begin = (u32)query_terms.size();
      cursor = line + 7;
      if (cursor < line_end && *cursor == '"')
        ++cursor;
      while (cursor < line_end && *cursor != '"') {
        query_terms.push_back(take_term(cursor, line_end, '"'));
        if (cursor < line_end && *cursor == ' ')
          ++cursor;
      }
    }
    if (parsed)
      query.n = (u32)query_terms.size() - query.begin;
    queries.push_back(query);
  }

  int threads = worker_count();
  Index index = build(docs_file, terms, query_terms, threads);
  malloc_trim(0);
  malloc_trim(0);

  constexpr int kSlot = 128;
  const size_t nqueries = queries.size();
  std::vector<char> slots(nqueries * kSlot);
  std::vector<int> lengths(nqueries);
  std::atomic<size_t> next{0};
  parallel(threads, [&](int) {
    Engine engine;
    engine.index = &index;
    engine.terms = &terms;
    engine.docs = &docs_file;
    engine.query_terms = &query_terms;
    engine.query_ops = &query_ops;
    for (;;) {
      size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= nqueries)
        break;
      const Query &query = queries[i];
      if (query.phrase)
        engine.phrase(query.begin, query.n);
      else
        engine.boolean(query.begin, query.n, query.op_at);
      lengths[i] = engine.format(slots.data() + i * kSlot);
    }
  });

  int fd = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0)
    die("open output");
  for (size_t i = 0; i < nqueries; i++) {
    const char *bytes = slots.data() + i * kSlot;
    size_t left = (size_t)(lengths[i]);
    while (left) {
      ssize_t wrote = ::write(fd, bytes, left);
      if (wrote < 0)
        die("write");
      bytes += wrote;
      left -= (size_t)(wrote);
    }
  }
  ::close(fd);
  return 0;
}
