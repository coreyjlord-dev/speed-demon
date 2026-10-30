// Track C (Join): quote-aware CSV parse, array join, country-local names.
//
// Names come from a pool of about 100k values. Each worker interns into
// a compact arena (a few MB) and compares hits against that arena, not
// against a random offset in the CSV. Name ids go into a per-country
// bucket and become a bitset only after the scan, one country at a time,
// so the bitset stays in L1. A global intern then gives every worker the
// same id for the same bytes, and the buckets are OR-ed. Amounts are cents.
// Region ids use a one-byte table. Finished CSV pages are dropped.

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <immintrin.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kEmpty = 0xffffffffu;
constexpr uint8_t kNoCountry = 0xff;

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
  if (hc >= 8) return static_cast<int>(hc / 2);
  return static_cast<int>(hc);
}

struct Mapped {
  int fd = -1;
  size_t len = 0;
  const char* data = nullptr;

  explicit Mapped(const std::string& path) {
    fd = ::open(path.c_str(), O_RDONLY);
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

void drop_pages(const char* base, size_t from, size_t to) {
  uintptr_t pa = (reinterpret_cast<uintptr_t>(base + from) + 4095u) & ~uintptr_t(4095);
  uintptr_t pb = reinterpret_cast<uintptr_t>(base + to) & ~uintptr_t(4095);
  if (pb > pa) madvise(reinterpret_cast<void*>(pa), pb - pa, MADV_DONTNEED);
}

bool is_bom(const char* p, const char* end) {
  return p + 3 <= end && static_cast<unsigned char>(p[0]) == 0xEF &&
         static_cast<unsigned char>(p[1]) == 0xBB &&
         static_cast<unsigned char>(p[2]) == 0xBF;
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

struct Slot {
  uint32_t hash = 0;
  uint32_t off = 0;
  uint32_t len = 0;
  uint32_t id = kEmpty;
};

struct Intern {
  std::string arena;
  std::vector<Slot> slots;
  std::vector<uint32_t> span_off;
  std::vector<uint32_t> span_len;
  uint32_t mask = 0;
  uint32_t used = 0;

  Intern() {
    slots.assign(1u << 18, Slot{});
    mask = (1u << 18) - 1;
    arena.reserve(1u << 22);
    span_off.reserve(1u << 17);
    span_len.reserve(1u << 17);
  }

  void rehash(uint32_t cap) {
    std::vector<Slot> ns(cap);
    uint32_t m = cap - 1;
    for (const Slot& sl : slots) {
      if (sl.id == kEmpty) continue;
      uint32_t i = sl.hash & m;
      while (ns[i].id != kEmpty) i = (i + 1) & m;
      ns[i] = sl;
    }
    slots.swap(ns);
    mask = m;
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

  uint32_t intern(const char* s, uint32_t n) {
    uint32_t h = mix(s, n);
    uint32_t i = h & mask;
    for (;;) {
      Slot& sl = slots[i];
      if (sl.id == kEmpty) {
        if ((used + 1) * 10 >= (mask + 1) * 7) {
          rehash((mask + 1) * 2);
          i = h & mask;
          continue;
        }
        sl.hash = h;
        sl.off = static_cast<uint32_t>(arena.size());
        sl.len = n;
        sl.id = used++;
        arena.append(s, n);
        span_off.push_back(sl.off);
        span_len.push_back(n);
        return sl.id;
      }
      if (sl.hash == h && sl.len == n &&
          (n == 0 || std::memcmp(arena.data() + sl.off, s, n) == 0)) {
        return sl.id;
      }
      i = (i + 1) & mask;
    }
  }

  const char* get(uint32_t id, uint32_t& n) const {
    n = span_len[id];
    return arena.data() + span_off[id];
  }
};

struct Local {
  std::vector<Agg> ag;
  Intern names;
  std::vector<std::vector<uint32_t>> buckets;

  explicit Local(size_t countries) : ag(countries), buckets(countries) {
    for (auto& v : buckets) v.reserve(2048);
  }
};

struct World {
  std::vector<uint8_t> region_country;
  std::vector<std::array<char, 3>> codes;
  int code_slot[26 * 26 * 26];
  uint32_t unmatched = 0;

  World() { std::fill(std::begin(code_slot), std::end(code_slot), -1); }

  uint32_t country_of(char a, char b, char c) {
    int s = (a - 'A') * 676 + (b - 'A') * 26 + (c - 'A');
    if (code_slot[s] >= 0) return static_cast<uint32_t>(code_slot[s]);
    uint32_t id = static_cast<uint32_t>(codes.size());
    if (id >= kNoCountry) die("too many countries");
    code_slot[s] = static_cast<int>(id);
    codes.push_back({a, b, c});
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
  for (const Row& r : rows) {
    uint32_t cid = w.country_of(r.a, r.b, r.c);
    w.region_country[r.id] = static_cast<uint8_t>(cid);
  }
  w.unmatched = static_cast<uint32_t>(w.codes.size());
}

inline uint32_t country_index(const World& w, uint32_t region) {
  if (region >= w.region_country.size()) return w.unmatched;
  uint8_t c = w.region_country[region];
  if (c == kNoCountry) return w.unmatched;
  return c;
}

inline const char* find_quote(const char* p, const char* end) {
  const uint64_t q = 0x2222222222222222ull;
  while (p + 8 <= end) {
    uint64_t v;
    std::memcpy(&v, p, 8);
    uint64_t x = v ^ q;
    uint64_t m = (x - 0x0101010101010101ull) & ~x & 0x8080808080808080ull;
    if (m) return p + static_cast<int>(__builtin_ctzll(m) >> 3);
    p += 8;
  }
  while (p < end && *p != '"') p++;
  return p;
}

const char* scan_quoted(const char* p, const char* end, const char** raw, uint32_t* unescaped_n,
                        std::string& scratch) {
  ++p;
  const char* s = p;
  const char* q = find_quote(p, end);
  if (q < end && q + 1 < end && q[1] == '"') {
    scratch.clear();
    uint32_t n = 0;
    while (p < end) {
      if (*p == '"') {
        if (p + 1 < end && p[1] == '"') {
          scratch.push_back('"');
          n++;
          p += 2;
        } else {
          ++p;
          break;
        }
      } else {
        scratch.push_back(*p);
        n++;
        ++p;
      }
    }
    *raw = scratch.data();
    *unescaped_n = n;
    return p;
  }
  *raw = s;
  *unescaped_n = static_cast<uint32_t>(q - s);
  return q < end ? q + 1 : q;
}

const char* parse_one(const char* p, const char* end, const World& w, Local& loc,
                      std::string& scratch) {
  uint64_t id = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    id = id * 10u + static_cast<uint32_t>(*p - '0');
    ++p;
  }
  ++p;

  const char* name_ptr;
  uint32_t name_len;
  if (*p == '"') {
    p = scan_quoted(p, end, &name_ptr, &name_len, scratch);
  } else {
    name_ptr = p;
    while (*p != ',') ++p;
    name_len = static_cast<uint32_t>(p - name_ptr);
  }
  // Copy into the arena before the note parser reuses scratch.
  uint32_t nid = loc.names.intern(name_ptr, name_len);
  ++p;

  uint32_t note_len = 0;
  if (*p == '"') {
    const char* raw = nullptr;
    p = scan_quoted(p, end, &raw, &note_len, scratch);
  } else {
    const char* s = p;
    while (*p != ',') ++p;
    note_len = static_cast<uint32_t>(p - s);
  }
  ++p;

  uint32_t region = 0;
  while (*p >= '0' && *p <= '9') {
    region = region * 10u + static_cast<uint32_t>(*p - '0');
    ++p;
  }
  ++p;

  int sign = 1;
  if (*p == '-') {
    sign = -1;
    ++p;
  }
  int64_t whole = 0;
  while (*p != '.') {
    whole = whole * 10 + (*p - '0');
    ++p;
  }
  ++p;
  int frac = (*p - '0') * 10 + (*(p + 1) - '0');
  p += 2;
  int64_t cents = sign * (whole * 100 + frac);
  if (p < end && *p == '\r') ++p;
  if (p < end && *p == '\n') ++p;

  uint32_t cid = country_index(w, region);
  Agg& a = loc.ag[cid];
  a.count++;
  a.id_sum += id;
  a.amount_sum += cents;
  if (cents < a.amin) a.amin = cents;
  if (cents > a.amax) a.amax = cents;
  a.name_bytes += name_len;
  a.note_bytes += note_len;
  loc.buckets[cid].push_back(nid);
  return p;
}

const char* find_record_start(const char* pos, const char* end) {
  for (const char* q = pos; q < end; q++) {
    if (*q != '\n') continue;
    const char* r = q + 1;
    if (is_bom(r, end)) r += 3;
    if (r < end && *r >= '1' && *r <= '9') return r;
  }
  return end;
}

void parse_range(const char* base, const char* p, const char* limit, const char* end,
                 const World& w, Local& loc) {
  std::string scratch;
  scratch.reserve(128);
  size_t dropped = static_cast<size_t>(p - base);
  while (p < end) {
    const char* q = p;
    if (is_bom(q, end)) q += 3;
    if (q >= limit || q >= end) break;
    p = parse_one(q, end, w, loc, scratch);
    size_t at = static_cast<size_t>(p - base);
    if (at - dropped >= (1u << 22)) {
      drop_pages(base, dropped, at);
      dropped = at;
    }
  }
  drop_pages(base, dropped, static_cast<size_t>(p - base));
}

uint32_t popcount_bits(const uint64_t* b, size_t n) {
  uint32_t c = 0;
  for (size_t i = 0; i < n; i++) c += static_cast<uint32_t>(__builtin_popcountll(b[i]));
  return c;
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

double mono() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

bool want_stats() {
  const char* e = std::getenv("SD_STATS");
  return e && *e && *e != '0';
}

}  // namespace

int main() {
  double t0 = mono();
  bool st = want_stats();
  std::string dir = env_or("DATA", "/data");
  std::string outp = std::string(env_or("OUT", "/out")) + "/aggregates.txt";
  Mapped regions(dir + "/regions.csv");
  Mapped records(dir + "/records.csv");

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
    starts[t] = find_record_start(cut, end);
  }
  for (int t = 1; t <= T; t++) {
    if (starts[t] < starts[t - 1]) starts[t] = starts[t - 1];
  }

  std::vector<Local> locals;
  locals.reserve(static_cast<size_t>(T));
  for (int t = 0; t < T; t++) locals.emplace_back(countries);

  std::vector<std::thread> pool;
  for (int t = 0; t < T; t++) {
    pool.emplace_back([&, t] {
      parse_range(data, starts[t], starts[t + 1], end, world, locals[t]);
    });
  }
  for (auto& th : pool) th.join();
  double t_parse = mono();

  std::vector<Agg> ag(countries);
  for (Local& loc : locals) {
    for (size_t c = 0; c < countries; c++) {
      Agg& dst = ag[c];
      const Agg& src = loc.ag[c];
      if (src.count == 0) continue;
      dst.count += src.count;
      dst.id_sum += src.id_sum;
      dst.name_bytes += src.name_bytes;
      dst.note_bytes += src.note_bytes;
      dst.amount_sum += src.amount_sum;
      if (src.amin < dst.amin) dst.amin = src.amin;
      if (src.amax > dst.amax) dst.amax = src.amax;
    }
  }

  std::vector<std::vector<uint32_t>> remap(static_cast<size_t>(T));
  Intern global;
  for (int t = 0; t < T; t++) {
    Intern& src = locals[t].names;
    remap[t].resize(src.used);
    for (uint32_t id = 0; id < src.used; id++) {
      uint32_t n = 0;
      const char* s = src.get(id, n);
      remap[t][id] = global.intern(s, n);
    }
    std::vector<Slot>().swap(src.slots);
    std::string().swap(src.arena);
  }
  double t_merge = mono();

  std::vector<uint32_t> distinct(countries);
  std::vector<uint32_t> work;
  work.reserve(countries);
  for (uint32_t c = 0; c < countries; c++) {
    if (ag[c].count) work.push_back(c);
  }
  std::sort(work.begin(), work.end(), [&](uint32_t a, uint32_t b) {
    size_t na = 0, nb = 0;
    for (int t = 0; t < T; t++) {
      na += locals[t].buckets[a].size();
      nb += locals[t].buckets[b].size();
    }
    return na > nb;
  });

  std::atomic<uint32_t> next{0};
  size_t words = (static_cast<size_t>(global.used) + 63) >> 6;
  if (words == 0) words = 1;
  pool.clear();
  for (int t = 0; t < T; t++) {
    pool.emplace_back([&] {
      std::vector<uint64_t> bits(words);
      for (;;) {
        uint32_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= work.size()) break;
        uint32_t c = work[i];
        std::memset(bits.data(), 0, words * sizeof(uint64_t));
        for (int k = 0; k < T; k++) {
          const auto& map = remap[k];
          for (uint32_t lid : locals[k].buckets[c]) {
            uint32_t g = map[lid];
            bits[g >> 6] |= 1ull << (g & 63);
          }
        }
        distinct[c] = popcount_bits(bits.data(), words);
      }
    });
  }
  for (auto& th : pool) th.join();
  double t_bits = mono();

  std::vector<uint32_t> order;
  order.reserve(world.codes.size());
  for (uint32_t c = 0; c < world.unmatched; c++) {
    if (ag[c].count) order.push_back(c);
  }
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return std::memcmp(world.codes[a].data(), world.codes[b].data(), 3) < 0;
  });
  if (ag[world.unmatched].count) order.push_back(world.unmatched);

  std::vector<char> out;
  out.reserve(order.size() * 80 + 64);
  for (uint32_t c : order) {
    const Agg& a = ag[c];
    if (c == world.unmatched) {
      out.insert(out.end(), "_UNMATCHED", "_UNMATCHED" + 10);
    } else {
      out.insert(out.end(), world.codes[c].data(), world.codes[c].data() + 3);
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
  if (st) {
    double t1 = mono();
    fprintf(stderr, "parse %.3f merge %.3f bits %.3f rest %.3f total %.3f names %u\n",
            t_parse - t0, t_merge - t_parse, t_bits - t_merge, t1 - t_bits, t1 - t0, global.used);
  }
  return 0;
}
