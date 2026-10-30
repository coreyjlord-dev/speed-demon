// Track C. One pass over records.csv.
//
// A record is id, name, note, region, amount. Names and notes are short, and
// quotes are escaped by doubling. memchr finds the next quote or comma, so
// each byte is touched once. Names are interned raw ("" is one-to-one with an
// escaped quote) in a shared table. A 16-bit tag rejects most probes. Ids are
// dense. Each thread appends those ids into per-country lists, then one bitset
// per country (it fits in L1) counts the distinct names. Lookups are queued
// 16 records ahead so the tag line is already in cache.
//
// Transparent huge pages are off. The box otherwise rounds dense tables up to
// 2 MB and the cgroup peak jumps.

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
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint8_t kNoCountry = 0xff;
constexpr uint32_t kSlots = 1u << 18;
constexpr uint32_t kMask = kSlots - 1;
constexpr uint32_t kMaxNames = 1u << 17;
constexpr uint32_t kWords = kMaxNames >> 6;
constexpr uint16_t kBusy = 1;

[[noreturn]] void die(const char* msg) {
  fprintf(stderr, "track-c: %s\n", msg);
  exit(1);
}

int worker_count() {
  if (const char* env = std::getenv("SD_THREADS")) {
    int n = std::atoi(env);
    if (n >= 1 && n <= 64) return n;
  }
  unsigned hc = std::thread::hardware_concurrency();
  if (hc == 0) return 1;
  if (hc > 12) return 12;
  return static_cast<int>(hc);
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
    len = static_cast<size_t>(st.st_size);
    if (len == 0) return;
    void* p = ::mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) die("mmap");
    data = static_cast<const char*>(p);
    madvise(const_cast<char*>(data), len, MADV_SEQUENTIAL);
  }
  ~Mapped() {
    if (data) munmap(const_cast<char*>(data), len);
    if (fd >= 0) close(fd);
  }
  Mapped(const Mapped&) = delete;
  Mapped& operator=(const Mapped&) = delete;
};

bool is_bom(const char* p, const char* end) {
  return p + 3 <= end && static_cast<unsigned char>(p[0]) == 0xEF &&
         static_cast<unsigned char>(p[1]) == 0xBB && static_cast<unsigned char>(p[2]) == 0xBF;
}

struct Agg {
  uint64_t count = 0;
  uint64_t id_sum = 0;
  uint64_t name_bytes = 0;
  uint64_t note_bytes = 0;
  int64_t amount_sum = 0;
  int64_t amin = INT64_MAX;
  int64_t amax = INT64_MIN;
};

struct World {
  std::vector<uint8_t> region_country;
  std::vector<char> codes;
  int code_slot[26 * 26 * 26];
  uint32_t unmatched = 0;

  World() { std::fill(std::begin(code_slot), std::end(code_slot), -1); }

  uint32_t country_of(char a, char b, char c) {
    int s = (a - 'A') * 676 + (b - 'A') * 26 + (c - 'A');
    if (code_slot[s] >= 0) return static_cast<uint32_t>(code_slot[s]);
    uint32_t id = static_cast<uint32_t>(codes.size() / 3);
    if (id >= kNoCountry) die("too many countries");
    code_slot[s] = static_cast<int>(id);
    codes.push_back(a);
    codes.push_back(b);
    codes.push_back(c);
    return id;
  }
};

void load_regions(const Mapped& file, World& w) {
  const char* p = file.data;
  const char* end = file.data + file.len;
  while (p < end && *p != '\n') p++;
  if (p < end) p++;
  uint32_t max_id = 0;
  struct Row {
    uint32_t id;
    char a, b, c;
  };
  std::vector<Row> rows;
  rows.reserve(file.len / 10);
  while (p < end) {
    uint32_t id = 0;
    while (p < end && *p >= '0' && *p <= '9') {
      id = id * 10u + static_cast<uint32_t>(*p - '0');
      p++;
    }
    if (p >= end || *p != ',') break;
    p++;
    char a = *p++;
    char b = *p++;
    char c = *p++;
    if (p < end && *p == '\n') p++;
    rows.push_back({id, a, b, c});
    if (id > max_id) max_id = id;
  }
  w.region_country.assign(static_cast<size_t>(max_id) + 1, kNoCountry);
  for (const Row& r : rows) w.region_country[r.id] = static_cast<uint8_t>(w.country_of(r.a, r.b, r.c));
  w.unmatched = static_cast<uint32_t>(w.codes.size() / 3);
}

inline uint32_t country_index(const World& w, uint32_t region) {
  if (region >= w.region_country.size()) return w.unmatched;
  uint8_t c = w.region_country[region];
  return c == kNoCountry ? w.unmatched : c;
}

inline uint64_t parse_digits(const char* s, const char* e) {
  uint64_t x = 0;
  for (; s < e; s++) x = x * 10 + static_cast<uint64_t>(*s - '0');
  return x;
}

struct Slot {
  uint32_t len = 0;
  uint32_t id = 0;
  char name[24]{};
  const char* extra = nullptr;
};

struct Names {
  std::atomic<uint16_t> tags[kSlots];
  Slot slots[kSlots];
  std::atomic<uint32_t> next{0};

  Names() {
    for (auto& t : tags) t.store(0, std::memory_order_relaxed);
  }

  static uint32_t mix(const char* s, uint32_t n) {
    uint64_t h = n;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) {
      uint64_t v;
      std::memcpy(&v, s + i, 8);
      h = _mm_crc32_u64(h, v);
    }
    if (i < n) {
      uint64_t v = 0;
      std::memcpy(&v, s + i, n - i);
      h = _mm_crc32_u64(h, v);
    }
    return static_cast<uint32_t>(h);
  }

  bool eq(const Slot& sl, const char* s, uint32_t n) const {
    if (sl.len != n) return false;
    if (n <= 24) return n == 0 || std::memcmp(sl.name, s, n) == 0;
    return std::memcmp(sl.name, s, 24) == 0 && std::memcmp(sl.extra, s + 24, n - 24) == 0;
  }

  uint32_t intern(const char* s, uint32_t n, uint32_t h) {
    uint16_t tag = static_cast<uint16_t>(h >> 16);
    if (tag < 2) tag = static_cast<uint16_t>(tag + 2);
    uint32_t i = h & kMask;
    for (;;) {
      uint16_t t = tags[i].load(std::memory_order_acquire);
      if (t == tag) {
        if (eq(slots[i], s, n)) return slots[i].id;
      } else if (t == 0) {
        uint16_t exp = 0;
        if (tags[i].compare_exchange_weak(exp, kBusy, std::memory_order_acq_rel)) {
          uint32_t id = next.fetch_add(1, std::memory_order_relaxed);
          if (id >= kMaxNames) die("name table full");
          Slot& sl = slots[i];
          sl.len = n;
          sl.id = id;
          uint32_t head = n < 24 ? n : 24;
          if (head) std::memcpy(sl.name, s, head);
          if (n > 24) {
            char* buf = new char[n - 24];
            std::memcpy(buf, s + 24, n - 24);
            sl.extra = buf;
          }
          tags[i].store(tag, std::memory_order_release);
          return id;
        }
        continue;
      } else if (t == kBusy) {
        _mm_pause();
        continue;
      }
      i = (i + 1) & kMask;
    }
  }
};

struct Pend {
  uint32_t country;
  const char* s;
  uint32_t n;
  uint32_t h;
};

struct View {
  const char* name = nullptr;
  uint32_t name_raw = 0;
  uint32_t name_un = 0;
  uint32_t note_un = 0;
  uint64_t id = 0;
  uint32_t region = 0;
  int64_t cents = 0;
  const char* next = nullptr;
};

// Closing quote, counting each doubled "" as one escaped quote.
inline const char* close_quote(const char* p, const char* end, int& esc) {
  esc = 0;
  while (p < end) {
    const char* q = static_cast<const char*>(std::memchr(p, '"', static_cast<size_t>(end - p)));
    if (!q) return end;
    if (q + 1 < end && q[1] == '"') {
      esc++;
      p = q + 2;
      continue;
    }
    return q;
  }
  return end;
}

inline int64_t parse_money(const char* s, const char* e) {
  int sign = 1;
  if (s < e && *s == '-') {
    sign = -1;
    s++;
  }
  const char* dot = s;
  while (dot < e && *dot != '.') dot++;
  uint64_t whole = parse_digits(s, dot);
  int frac = 0;
  if (dot + 2 < e) frac = (dot[1] - '0') * 10 + (dot[2] - '0');
  return sign * (static_cast<int64_t>(whole) * 100 + frac);
}

View parse_at(const char* p, const char* end) {
  View v;
  if (is_bom(p, end)) p += 3;
  const char* id_b = p;
  while (p < end && *p != ',') p++;
  v.id = parse_digits(id_b, p);
  if (p < end) p++;

  if (p < end && *p == '"') {
    const char* ns = ++p;
    int esc = 0;
    const char* q = close_quote(p, end, esc);
    v.name = ns;
    v.name_raw = static_cast<uint32_t>(q - ns);
    v.name_un = v.name_raw - static_cast<uint32_t>(esc);
    p = q < end ? q + 1 : end;
  } else {
    v.name = p;
    while (p < end && *p != ',') p++;
    v.name_raw = v.name_un = static_cast<uint32_t>(p - v.name);
  }
  if (p < end && *p == ',') p++;

  if (p < end && *p == '"') {
    const char* ns = ++p;
    int esc = 0;
    const char* q = close_quote(p, end, esc);
    v.note_un = static_cast<uint32_t>(q - ns) - static_cast<uint32_t>(esc);
    p = q < end ? q + 1 : end;
  } else {
    const char* ns = p;
    while (p < end && *p != ',') p++;
    v.note_un = static_cast<uint32_t>(p - ns);
  }
  if (p < end && *p == ',') p++;

  const char* rb = p;
  while (p < end && *p != ',') p++;
  v.region = static_cast<uint32_t>(parse_digits(rb, p));
  if (p < end && *p == ',') p++;

  const char* ab = p;
  while (p < end && *p != '\n') p++;
  const char* ae = p;
  if (ae > ab && ae[-1] == '\r') ae--;
  v.cents = parse_money(ab, ae);
  v.next = p + (p < end);
  return v;
}

const char* find_crlf(const char* p, const char* end) {
  while (p + 1 < end) {
    if (p[0] == '\r' && p[1] == '\n') return p + 2;
    p++;
  }
  return end;
}

void append_u64(std::vector<char>& o, uint64_t x) {
  char tmp[32];
  int n = 0;
  do {
    tmp[n++] = static_cast<char>('0' + x % 10);
    x /= 10;
  } while (x);
  while (n--) o.push_back(tmp[n]);
}

void append_money(std::vector<char>& o, int64_t cents) {
  if (cents < 0) {
    o.push_back('-');
    cents = -cents;
  }
  append_u64(o, static_cast<uint64_t>(cents / 100));
  o.push_back('.');
  int f = static_cast<int>(cents % 100);
  o.push_back(static_cast<char>('0' + f / 10));
  o.push_back(static_cast<char>('0' + f % 10));
}

void write_all(int fd, const char* p, size_t n) {
  while (n) {
    ssize_t k = ::write(fd, p, n);
    if (k < 0) die("write");
    p += k;
    n -= static_cast<size_t>(k);
  }
}

const char* env_or(const char* key, const char* fallback) {
  const char* v = std::getenv(key);
  return v && *v ? v : fallback;
}

uint32_t popcount_words(const uint64_t* b) {
  uint32_t n = 0;
  for (uint32_t i = 0; i < kWords; i++) n += static_cast<uint32_t>(__builtin_popcountll(b[i]));
  return n;
}

}  // namespace

int main() {
  tune_alloc();
  std::string dir = env_or("DATA", "/data");
  std::string outp = std::string(env_or("OUT", "/out")) + "/aggregates.txt";
  Mapped regions((dir + "/regions.csv").c_str());
  Mapped records((dir + "/records.csv").c_str());

  World world;
  load_regions(regions, world);
  size_t countries = static_cast<size_t>(world.unmatched) + 1;

  const char* data = records.data;
  const char* end = records.data + records.len;
  const char* p = data;
  if (is_bom(p, end)) p += 3;
  while (p < end && *p != '\n') p++;
  if (p < end) p++;
  const char* first = p;

  int T = worker_count();
  std::vector<const char*> starts(static_cast<size_t>(T) + 1);
  starts[0] = first;
  starts[T] = end;
  for (int t = 1; t < T; t++) {
    const char* cut = data + records.len * static_cast<size_t>(t) / static_cast<size_t>(T);
    if (cut < first) cut = first;
    starts[t] = find_crlf(cut, end);
  }
  for (int t = 1; t <= T; t++) {
    if (starts[t] < starts[t - 1]) starts[t] = starts[t - 1];
  }

  Names* names = new Names;
  std::vector<std::vector<std::vector<uint32_t>>> buckets(static_cast<size_t>(T));
  for (int t = 0; t < T; t++) buckets[t].resize(countries);
  std::vector<std::vector<Agg>> ags(static_cast<size_t>(T), std::vector<Agg>(countries));

  std::vector<std::thread> pool;
  for (int t = 0; t < T; t++) {
    pool.emplace_back([&, t] {
      std::vector<Agg>& ag = ags[t];
      auto& bk = buckets[t];
      Pend ring[16];
      int head = 0;
      int filled = 0;
      auto drain_one = [&] {
        Pend e = ring[head];
        head = (head + 1) & 15;
        filled--;
        uint32_t id = names->intern(e.s, e.n, e.h);
        bk[e.country].push_back(id);
      };
      const char* q = starts[t];
      const char* limit = starts[t + 1];
      while (q < limit && q < end) {
        if (q >= limit || q >= end) break;
        _mm_prefetch(q + 256, _MM_HINT_T0);
        View v = parse_at(q, end);
        uint32_t cid = country_index(world, v.region);
        Agg& a = ag[cid];
        a.count++;
        a.id_sum += v.id;
        a.amount_sum += v.cents;
        if (v.cents < a.amin) a.amin = v.cents;
        if (v.cents > a.amax) a.amax = v.cents;
        a.name_bytes += v.name_un;
        a.note_bytes += v.note_un;
        uint32_t h = Names::mix(v.name, v.name_raw);
        _mm_prefetch(reinterpret_cast<const char*>(&names->tags[h & kMask]), _MM_HINT_T0);
        _mm_prefetch(reinterpret_cast<const char*>(&names->slots[h & kMask]), _MM_HINT_T0);
        if (filled == 16) drain_one();
        ring[(head + filled) & 15] = Pend{cid, v.name, v.name_raw, h};
        filled++;
        q = v.next;
      }
      while (filled) drain_one();
    });
  }
  for (auto& th : pool) th.join();

  std::vector<uint32_t> distinct(countries);
  {
    std::atomic<uint32_t> cursor{0};
    std::vector<std::thread> counters;
    int ct = T;
    for (int t = 0; t < ct; t++) {
      counters.emplace_back([&, t] {
        (void)t;
        std::vector<uint64_t> bits(kWords);
        for (;;) {
          uint32_t c = cursor.fetch_add(1, std::memory_order_relaxed);
          if (c >= countries) break;
          bool any = false;
          for (int k = 0; k < T; k++) {
            if (!buckets[k][c].empty()) {
              any = true;
              break;
            }
          }
          if (!any) continue;
          std::memset(bits.data(), 0, kWords * sizeof(uint64_t));
          for (int k = 0; k < T; k++) {
            for (uint32_t id : buckets[k][c]) bits[id >> 6] |= 1ull << (id & 63);
          }
          distinct[c] = popcount_words(bits.data());
        }
      });
    }
    for (auto& th : counters) th.join();
  }

  std::vector<Agg> ag(countries);
  for (int t = 0; t < T; t++) {
    for (size_t c = 0; c < countries; c++) {
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

  std::vector<uint32_t> order;
  order.reserve(world.unmatched);
  for (uint32_t c = 0; c < world.unmatched; c++)
    if (ag[c].count) order.push_back(c);
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return std::memcmp(world.codes.data() + a * 3, world.codes.data() + b * 3, 3) < 0;
  });
  if (ag[world.unmatched].count) order.push_back(world.unmatched);

  std::vector<char> out;
  out.reserve(order.size() * 80 + 64);
  for (uint32_t c : order) {
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
