// Track C. One parallel parse, one shared name table, one country bitset.
// Quoted fields are scanned 32 bytes at a time. Distinct names are marked
// with a relaxed atomic or, so the per-record id lists are not stored.

#include <algorithm>
#include <atomic>
#include <climits>
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
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace std;
using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;

constexpr u8 kNoCountry = 0xff;
// 2^17 slots of 24 bytes is 3 MB, so the open-addressed table stays in L2.
// The bitset covers 102400 ids. This file has 100520 names, so the extra
// slots in the hash table do not each need a bit.
constexpr u32 kSlots = 1u << 17;
constexpr u32 kMask = kSlots - 1;
constexpr u32 kMaxNames = 102400;
constexpr u32 kWords = kMaxNames >> 6;
constexpr u16 kBusy = 1;
constexpr int kRing = 16;

[[noreturn]] void die(const char* m) {
  fprintf(stderr, "track-c: %s\n", m);
  exit(1);
}

int worker_count() {
  if (const char* e = getenv("SD_THREADS")) {
    int n = atoi(e);
    if (n >= 1 && n <= 64) return n;
  }
  unsigned hc = thread::hardware_concurrency();
  if (!hc) return 1;
  if (hc > 12) return 12;
  return (int)hc;
}

void tune_alloc() {
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
  mallopt(M_ARENA_MAX, 2);
  mallopt(M_MMAP_THRESHOLD, 1 << 20);
}

struct Mapped {
  int fd = -1;
  size_t len = 0;
  const char* data = nullptr;

  explicit Mapped(const char* path) {
    fd = ::open(path, O_RDONLY);
    if (fd < 0) die("open input");
    struct stat st {};
    if (fstat(fd, &st) != 0) die("fstat");
    len = (size_t)st.st_size;
    if (!len) return;
    void* p = mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) die("mmap");
    data = (const char*)p;
    // MAP_POPULATE and MADV_WILLNEED were slower than a sequential hint.
    madvise((char*)data, len, MADV_SEQUENTIAL);
  }

  ~Mapped() {
    if (data) munmap((char*)data, len);
    if (fd >= 0) close(fd);
  }

  Mapped(const Mapped&) = delete;
  Mapped& operator=(const Mapped&) = delete;
};

bool is_bom(const char* p, const char* end) {
  return p + 3 <= end && (u8)p[0] == 0xEF && (u8)p[1] == 0xBB && (u8)p[2] == 0xBF;
}

struct Agg {
  u64 count = 0, id_sum = 0, name_bytes = 0, note_bytes = 0;
  int64_t amount_sum = 0, amin = INT64_MAX, amax = INT64_MIN;
};

struct World {
  vector<u8> region_country;
  vector<char> codes;
  int code_slot[26 * 26 * 26];
  u32 unmatched = 0;

  World() { fill(begin(code_slot), end(code_slot), -1); }

  u32 country_of(char a, char b, char c) {
    int s = (a - 'A') * 676 + (b - 'A') * 26 + (c - 'A');
    if (code_slot[s] >= 0) return (u32)code_slot[s];
    u32 id = (u32)(codes.size() / 3);
    if (id >= kNoCountry) die("too many countries");
    code_slot[s] = (int)id;
    codes.push_back(a);
    codes.push_back(b);
    codes.push_back(c);
    return id;
  }
};

void load_regions(const Mapped& file, World& w) {
  const char* p = file.data;
  const char* end = file.data + file.len;
  while (p < end && *p != '\n') ++p;
  if (p < end) ++p;
  u32 max_id = 0;
  struct Row {
    u32 id;
    char a, b, c;
  };
  vector<Row> rows;
  rows.reserve(file.len / 10);
  while (p < end) {
    u32 id = 0;
    while (p < end && *p >= '0' && *p <= '9') {
      id = id * 10u + (u32)(*p - '0');
      ++p;
    }
    if (p >= end || *p != ',') break;
    ++p;
    char a = *p++;
    char b = *p++;
    char c = *p++;
    if (p < end && *p == '\n') ++p;
    rows.push_back({id, a, b, c});
    if (id > max_id) max_id = id;
  }
  w.region_country.assign((size_t)max_id + 1, kNoCountry);
  for (const Row& r : rows) w.region_country[r.id] = (u8)w.country_of(r.a, r.b, r.c);
  w.unmatched = (u32)(w.codes.size() / 3);
}

inline u32 country_index(const World& w, u32 region) {
  if (region >= w.region_country.size()) return w.unmatched;
  u8 c = w.region_country[region];
  return c == kNoCountry ? w.unmatched : c;
}

inline u64 parse_digits(const char* s, const char* e) {
  u64 x = 0;
  for (; s < e; ++s) x = x * 10 + (u64)(*s - '0');
  return x;
}

// Open-addressed slot. state is the hash (never 0 or 1), 1 while claimed.
// Up to 16 name bytes sit in the slot. Longer names are one arena pointer.
struct Slot {
  atomic<u32> state;
  u32 meta;
  char bytes[16];
};
static_assert(sizeof(Slot) == 24, "name slot");

struct Arena {
  struct Block {
    Block* next = nullptr;
    u32 used = 0;
    char data[1 << 16]{};
  };
  Block* head = nullptr;

  char* alloc(u32 n) {
    if (!head || head->used + n > sizeof(head->data)) {
      Block* b = new Block;
      b->next = head;
      head = b;
    }
    char* p = head->data + head->used;
    head->used += n;
    return p;
  }
};

struct Names {
  Slot slots[kSlots];
  atomic<u32> next{0};

  Names() {
    for (auto& sl : slots) sl.state.store(0, memory_order_relaxed);
  }

  static u32 slot_len(const Slot& sl) { return sl.meta >> 18; }
  static u32 slot_id(const Slot& sl) { return sl.meta & ((1u << 18) - 1); }

  static bool eq(const Slot& sl, const char* s, u32 n) {
    if (slot_len(sl) != n) return false;
    if (n <= 16) return n == 0 || memcmp(sl.bytes, s, n) == 0;
    const char* stored;
    memcpy(&stored, sl.bytes, sizeof(stored));
    return memcmp(stored, s, n) == 0;
  }

  static void keep(Slot& sl, const char* s, u32 n, u32 id, Arena& arena) {
    sl.meta = id | (n << 18);
    if (n <= 16) {
      if (n) memcpy(sl.bytes, s, n);
      return;
    }
    char* copy = arena.alloc(n);
    memcpy(copy, s, n);
    memcpy(sl.bytes, &copy, sizeof(copy));
  }

  u32 intern(const char* s, u32 n, u32 h, Arena& arena) {
    u32 i = h & kMask;
    u32 advanced = 0;
    for (;;) {
      u32 st = slots[i].state.load(memory_order_acquire);
      if (st == h) {
        if (eq(slots[i], s, n)) return slot_id(slots[i]);
      } else if (st == 0) {
        u32 exp = 0;
        if (slots[i].state.compare_exchange_weak(exp, kBusy, memory_order_acq_rel)) {
          u32 id = next.fetch_add(1, memory_order_relaxed);
          if (id >= kMaxNames) die("name table full");
          keep(slots[i], s, n, id, arena);
          slots[i].state.store(h, memory_order_release);
          return id;
        }
        continue;
      } else if (st == kBusy) {
        _mm_pause();
        continue;
      }
      i = (i + 1) & kMask;
      if (++advanced >= kSlots) die("name table full");
    }
  }
};

struct View {
  const char* name = nullptr;
  u32 name_raw = 0, name_un = 0, note_un = 0, region = 0;
  u32 name_hash = 0;
  u64 id = 0;
  int64_t cents = 0;
  const char* next = nullptr;
};

inline u64 crc_span(u64 h, const char* s, const char* e) {
  while (s + 8 <= e) {
    u64 v;
    memcpy(&v, s, 8);
    h = _mm_crc32_u64(h, v);
    s += 8;
  }
  if (s < e) {
    u64 v = 0;
    memcpy(&v, s, (size_t)(e - s));
    h = _mm_crc32_u64(h, v);
  }
  return h;
}

inline u32 fold_hash(u64 h, u32 n) {
  h = _mm_crc32_u64(h, n);
  if ((u32)h < 2) h += 2;
  return (u32)h;
}

// AVX2 step to the next quote. Notes also stop on a newline.
inline const char* find_delim(const char* p, const char* end, bool notes) {
  const __m256i quote = _mm256_set1_epi8('"');
  const __m256i brk = _mm256_set1_epi8('\n');
  while (p + 32 <= end) {
    __m256i v = _mm256_loadu_si256((const __m256i*)p);
    int mq = _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, quote));
    int mn = notes ? _mm256_movemask_epi8(_mm256_cmpeq_epi8(v, brk)) : 0;
    int m = mq | mn;
    if (m) return p + __builtin_ctz(m);
    p += 32;
  }
  while (p < end && *p != '"' && !(notes && *p == '\n')) ++p;
  return p;
}

// Quoted field. Names are hashed in this same scan. "" is an escaped quote.
inline const char* read_quoted(const char* p, const char* end, const char*& raw, u32& raw_n,
                              u32& un_n, u32* hash_out) {
  ++p;
  raw = p;
  u64 h = 0;
  int esc = 0;
  while (p < end) {
    const char* d = find_delim(p, end, hash_out == nullptr);
    if (hash_out) h = crc_span(h, p, d);
    if (d >= end) break;
    if (*d == '\n') {
      p = d + 1;
      continue;
    }
    if (d + 1 < end && d[1] == '"') {
      ++esc;
      if (hash_out) h = crc_span(h, d, d + 2);
      p = d + 2;
      continue;
    }
    raw_n = (u32)(d - raw);
    un_n = raw_n - (u32)esc;
    if (hash_out) *hash_out = fold_hash(h, raw_n);
    return d + 1;
  }
  raw_n = (u32)(end - raw);
  un_n = raw_n - (u32)esc;
  if (hash_out) *hash_out = fold_hash(h, raw_n);
  return end;
}

inline const char* read_bare(const char* p, const char* end, const char*& raw, u32& n) {
  raw = p;
  while (p < end && *p != ',' && *p != '\n') ++p;
  n = (u32)(p - raw);
  return p;
}

inline int64_t parse_money(const char* s, const char* e) {
  int sign = 1;
  if (s < e && *s == '-') {
    sign = -1;
    ++s;
  }
  const char* dot = s;
  while (dot < e && *dot != '.') ++dot;
  u64 whole = parse_digits(s, dot);
  int frac = 0;
  if (dot + 2 < e) frac = (dot[1] - '0') * 10 + (dot[2] - '0');
  return sign * ((int64_t)whole * 100 + frac);
}

inline View parse_record(const char* p, const char* end) {
  View v;
  if (is_bom(p, end)) p += 3;
  const char* id_b = p;
  while (p < end && *p != ',') ++p;
  v.id = parse_digits(id_b, p);
  if (p < end) ++p;
  if (p < end && *p == '"') {
    p = read_quoted(p, end, v.name, v.name_raw, v.name_un, &v.name_hash);
  } else {
    p = read_bare(p, end, v.name, v.name_raw);
    v.name_un = v.name_raw;
    v.name_hash = fold_hash(crc_span(0, v.name, v.name + v.name_raw), v.name_raw);
  }
  if (p < end && *p == ',') ++p;
  u32 note_raw = 0;
  const char* note = nullptr;
  if (p < end && *p == '"') {
    p = read_quoted(p, end, note, note_raw, v.note_un, nullptr);
  } else {
    p = read_bare(p, end, note, note_raw);
    v.note_un = note_raw;
  }
  if (p < end && *p == ',') ++p;
  const char* rb = p;
  while (p < end && *p != ',') ++p;
  v.region = (u32)parse_digits(rb, p);
  if (p < end && *p == ',') ++p;
  const char* ab = p;
  while (p < end && *p != '\n') ++p;
  const char* ae = p;
  if (ae > ab && ae[-1] == '\r') --ae;
  v.cents = parse_money(ab, ae);
  v.next = p + (p < end);
  return v;
}

const char* find_crlf(const char* p, const char* end) {
  while (p + 1 < end) {
    const void* hit = memchr(p, '\r', (size_t)(end - p));
    if (!hit) return end;
    const char* c = (const char*)hit;
    if (c + 1 < end && c[1] == '\n') return c + 2;
    p = c + 1;
  }
  return end;
}

void append_u64(vector<char>& o, u64 x) {
  char tmp[32];
  int n = 0;
  do tmp[n++] = (char)('0' + x % 10);
  while (x /= 10);
  while (n--) o.push_back(tmp[n]);
}

void append_money(vector<char>& o, int64_t cents) {
  if (cents < 0) {
    o.push_back('-');
    cents = -cents;
  }
  append_u64(o, (u64)(cents / 100));
  o.push_back('.');
  int f = (int)(cents % 100);
  o.push_back((char)('0' + f / 10));
  o.push_back((char)('0' + f % 10));
}

void write_all(int fd, const char* p, size_t n) {
  while (n) {
    ssize_t k = ::write(fd, p, n);
    if (k < 0) die("write");
    p += k;
    n -= (size_t)k;
  }
}

const char* env_or(const char* key, const char* fb) {
  const char* v = getenv(key);
  return v && *v ? v : fb;
}

u32 popcount_row(const u64* b) {
  u32 n = 0;
  for (u32 i = 0; i < kWords; ++i) n += (u32)__builtin_popcountll(b[i]);
  return n;
}

void mark_name(u64* bits, u32 country, u32 id) {
  u64* word = bits + (size_t)country * kWords + (id >> 6);
  __atomic_fetch_or(word, 1ull << (id & 63), __ATOMIC_RELAXED);
}

struct Pend {
  u32 country;
  const char* s;
  u32 n, h;
};

}  // namespace

int main() {
  tune_alloc();
  string dir = env_or("DATA", "/data");
  string outp = string(env_or("OUT", "/out")) + "/aggregates.txt";
  Mapped regions((dir + "/regions.csv").c_str());
  Mapped records((dir + "/records.csv").c_str());
  World world;
  load_regions(regions, world);
  size_t countries = (size_t)world.unmatched + 1;
  const char* data = records.data;
  const char* end = records.data + records.len;
  const char* p = data;
  if (is_bom(p, end)) p += 3;
  while (p < end && *p != '\n') ++p;
  if (p < end) ++p;
  const char* first = p;
  int T = worker_count();
  vector<const char*> starts((size_t)T + 1);
  starts[0] = first;
  starts[T] = end;
  for (int t = 1; t < T; ++t) {
    const char* cut = data + records.len * (size_t)t / (size_t)T;
    if (cut < first) cut = first;
    starts[t] = find_crlf(cut, end);
  }
  for (int t = 1; t <= T; ++t) {
    if (starts[t] < starts[t - 1]) starts[t] = starts[t - 1];
  }

  vector<vector<Agg>> ags((size_t)T, vector<Agg>(countries));
  // Private bitsets were larger and slower at every thread count tried.
  // One shared row set stays in cache; the or is relaxed.
  vector<u64> bits(countries * kWords);
  vector<Arena> arenas((size_t)T);
  Names* names = new Names;
  vector<thread> pool;
  for (int t = 0; t < T; ++t) {
    pool.emplace_back([&, t] {
      vector<Agg>& ag = ags[t];
      Arena& arena = arenas[t];
      Pend ring[kRing];
      int head = 0, filled = 0;
      auto drain_one = [&] {
        Pend e = ring[head];
        head = (head + 1) & (kRing - 1);
        --filled;
        u32 id = names->intern(e.s, e.n, e.h, arena);
        mark_name(bits.data(), e.country, id);
      };
      const char* q = starts[t];
      const char* limit = starts[t + 1];
      while (q < limit && q < end) {
        _mm_prefetch(q + 256, _MM_HINT_T0);
        View v = parse_record(q, end);
        u32 cid = country_index(world, v.region);
        Agg& a = ag[cid];
        a.count++;
        a.id_sum += v.id;
        a.amount_sum += v.cents;
        if (v.cents < a.amin) a.amin = v.cents;
        if (v.cents > a.amax) a.amax = v.cents;
        a.name_bytes += v.name_un;
        a.note_bytes += v.note_un;
        u32 h = v.name_hash;
        _mm_prefetch(&names->slots[h & kMask], _MM_HINT_T0);
        if (filled == kRing) drain_one();
        ring[(head + filled) & (kRing - 1)] = Pend{cid, v.name, v.name_raw, h};
        ++filled;
        q = v.next;
      }
      while (filled) drain_one();
    });
  }
  for (auto& th : pool) th.join();
  delete names;

  vector<u32> distinct(countries);
  for (size_t c = 0; c < countries; ++c) distinct[c] = popcount_row(bits.data() + c * kWords);

  vector<Agg> ag(countries);
  for (int t = 0; t < T; ++t) {
    for (size_t c = 0; c < countries; ++c) {
      const Agg& src = ags[t][c];
      if (src.count == 0) continue;
      Agg& dst = ag[c];
      dst.count += src.count;
      dst.id_sum += src.id_sum;
      dst.name_bytes += src.name_bytes;
      dst.note_bytes += src.note_bytes;
      dst.amount_sum += src.amount_sum;
      if (src.amin < dst.amin) dst.amin = src.amin;
      if (src.amax > dst.amax) dst.amax = src.amax;
    }
  }

  vector<u32> order;
  order.reserve(world.unmatched);
  for (u32 c = 0; c < world.unmatched; ++c) {
    if (ag[c].count) order.push_back(c);
  }
  sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    return memcmp(world.codes.data() + a * 3, world.codes.data() + b * 3, 3) < 0;
  });
  if (ag[world.unmatched].count) order.push_back(world.unmatched);

  vector<char> out;
  out.reserve(order.size() * 80 + 64);
  for (u32 c : order) {
    const Agg& a = ag[c];
    if (c == world.unmatched) {
      out.insert(out.end(), "_UNMATCHED", "_UNMATCHED" + 10);
    } else {
      out.insert(out.end(), world.codes.data() + c * 3, world.codes.data() + c * 3 + 3);
    }
    out.push_back(' ');
    append_u64(out, a.count);
    out.push_back(' ');
    append_u64(out, a.id_sum);
    out.push_back(' ');
    append_money(out, a.amount_sum);
    out.push_back(' ');
    append_money(out, a.amin);
    out.push_back(' ');
    append_money(out, a.amax);
    out.push_back(' ');
    append_u64(out, distinct[c]);
    out.push_back(' ');
    append_u64(out, a.name_bytes);
    out.push_back(' ');
    append_u64(out, a.note_bytes);
    out.push_back('\n');
  }

  int fd = ::open(outp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) die("open output");
  write_all(fd, out.data(), out.size());
  close(fd);
  return 0;
}
