// Track A: windowed map, one uint32 per latency, streamed output.
// Hour is a fixed offset in the 72-hour window. Nearest rank is
// index (p * n + 99) / 100 - 1. A 5xx is status[0] == '5'.

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
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef MADV_POPULATE_READ
#define MADV_POPULATE_READ 22
#endif

namespace {

constexpr int kHours = 72;
constexpr uint32_t kCap = 2048;
constexpr uint32_t kSlots = 4096;

[[noreturn]] void die(const char* msg) {
  fprintf(stderr, "track-a: %s\n", msg);
  exit(1);
}

int worker_count() {
  if (const char* env = std::getenv("SD_THREADS")) {
    int n = std::atoi(env);
    if (n >= 1 && n <= 64) return n;
  }
  unsigned hc = std::thread::hardware_concurrency();
  if (hc == 0) return 1;
  if (hc >= 8) return 6;
  return static_cast<int>(hc);
}

struct Mapped {
  int fd = -1;
  size_t len = 0;
  const char* data = nullptr;
  explicit Mapped(const std::string& path) {
    fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) die("open");
    struct stat st {};
    if (fstat(fd, &st) != 0) die("fstat");
    len = static_cast<size_t>(st.st_size);
    if (!len) return;
    void* p = ::mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (p == MAP_FAILED) die("mmap");
    data = static_cast<const char*>(p);
    madvise(const_cast<char*>(data), len, MADV_RANDOM);
  }
  ~Mapped() {
    if (data) munmap(const_cast<char*>(data), len);
    if (fd >= 0) close(fd);
  }
};

void advise(const char* base, size_t from, size_t to, int how) {
  uintptr_t a = reinterpret_cast<uintptr_t>(base + from) & ~uintptr_t(4095);
  uintptr_t b = reinterpret_cast<uintptr_t>(base + to) & ~uintptr_t(4095);
  if (b > a) madvise(reinterpret_cast<void*>(a), b - a, how);
}

struct Slot {
  uint64_t h = 0;
  uint32_t off = 0;
  uint32_t len = 0;
  uint32_t id = 0;
};

struct Loc {
  std::string bytes;
  std::vector<Slot> slots;
  uint32_t mask = kSlots - 1;
  uint32_t used = 0;
  uint32_t nids = 0;
  const char* blob = nullptr;
  std::vector<uint32_t> n;
  std::vector<uint32_t> e;
  explicit Loc(bool counts = true) {
    bytes.reserve(1 << 16);
    slots.resize(kSlots);
    if (counts) {
      n.assign(static_cast<size_t>(kHours) * kCap, 0);
      e.assign(static_cast<size_t>(kHours) * kCap, 0);
    }
    blob = bytes.data();
  }
  uint32_t intern(const char* s, uint32_t len, uint64_t h, bool insert) {
    uint32_t i = static_cast<uint32_t>(h) & mask;
    for (;;) {
      Slot& sl = slots[i];
      if (sl.h == h && sl.len == len && std::memcmp(blob + sl.off, s, len) == 0) return sl.id;
      if (!sl.h) {
        if (!insert) die("lookup");
        if (nids >= kCap) die("endpoints");
        sl.h = h;
        sl.off = static_cast<uint32_t>(bytes.size());
        sl.len = len;
        sl.id = nids++;
        bytes.append(s, len);
        blob = bytes.data();
        used++;
        return sl.id;
      }
      i = (i + 1) & mask;
    }
  }
};

struct G {
  uint32_t n = 0;
  uint32_t err = 0;
  uint32_t off = 0;
  uint32_t p50 = 0;
  uint32_t p95 = 0;
  uint32_t p99 = 0;
};

inline int hour_of(const char* ts) {
  int month = ts[6] - '0';
  int day = (ts[8] - '0') * 10 + (ts[9] - '0');
  int hour = (ts[11] - '0') * 10 + (ts[12] - '0');
  return (month ? day + 1 : day - 30) * 24 + hour - 18;
}

inline int method_len(const char* p) {
  unsigned c = static_cast<unsigned char>(p[0]);
  if (c == 'G') return 3;
  if (c == 'P') {
    unsigned d = static_cast<unsigned char>(p[1]);
    return d == 'O' ? 4 : d == 'A' ? 5 : 3;
  }
  if (c == 'D') return 6;
  if (c == 'H') return 4;
  return 7;
}

inline uint64_t mix(uint64_t h, uint32_t n) {
  h ^= static_cast<uint64_t>(n) * 0x9E3779B185EBCA87ull;
  h *= 0x100000001B3ull;
  return h ? h : 1;
}

inline uint64_t hash_ep(const char* s, uint32_t n) {
  uint64_t h = 0;
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t v;
    std::memcpy(&v, s + i, 8);
    h ^= v;
  }
  if (i < n) {
    uint64_t v = 0;
    std::memcpy(&v, s + i, n - i);
    h ^= v;
  }
  return mix(h, n);
}

inline uint32_t parse_lat(const char* digs, const char* nl, const char* file_end) {
  int k = static_cast<int>(nl - digs);
  if (k <= 8 && digs + 8 <= file_end) {
    uint64_t v;
    std::memcpy(&v, digs, 8);
    uint64_t mask = k == 8 ? ~0ull : (1ull << (k * 8)) - 1ull;
    v &= mask & 0x0F0F0F0F0F0F0F0Full;
    uint32_t x = v & 255u;
    x = x * 10u + ((v >> 8) & 255u);
    x = x * 10u + ((v >> 16) & 255u);
    x = x * 10u + ((v >> 24) & 255u);
    x = x * 10u + static_cast<uint32_t>((v >> 32) & 255u);
    x = x * 10u + static_cast<uint32_t>((v >> 40) & 255u);
    x = x * 10u + static_cast<uint32_t>((v >> 48) & 255u);
    x = x * 10u + static_cast<uint32_t>(v >> 56);
    static const uint32_t div10[9] = {100000000u, 10000000u, 1000000u, 100000u, 10000u,
                                       1000u,      100u,      10u,      1u};
    return x / div10[k];
  }
  uint32_t x = 0;
  for (const char* p = digs; p < nl; ++p) x = x * 10u + static_cast<uint32_t>(*p - '0');
  return x;
}

inline void on_line(Loc& t, const char* line, const char* nl, const char* file_end, bool fill,
                    uint32_t* lats) {
  const char* win = nl - 16;
  __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(win));
  uint32_t sm = static_cast<uint32_t>(_mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_set1_epi8(' '))));
  int idx = 31 - __builtin_clz(sm);
  const char* lat_sp = win + idx;
  uint32_t err = lat_sp[-3] == '5';
  const char* mth = line + 28;
  const char* ep = mth + method_len(mth) + 1;
  uint32_t elen = static_cast<uint32_t>((lat_sp - 4) - ep);
  int hour = hour_of(line);
  uint64_t h = hash_ep(ep, elen);
  uint32_t id = t.intern(ep, elen, h, !fill);
  if (!fill) {
    t.n[static_cast<size_t>(hour) * kCap + id]++;
    t.e[static_cast<size_t>(hour) * kCap + id] += err;
  } else {
    lats[t.n[static_cast<size_t>(hour) * kCap + id]++] = parse_lat(lat_sp + 1, nl, file_end);
  }
}

void parse_window(Loc& t, const char* b, const char* e, const char* file_end, bool fill, uint32_t* lats) {
  const char* line = b;
  const char* p = b;
  const __m256i nlv = _mm256_set1_epi8('\n');
  while (p + 32 <= e) {
    uint32_t m = static_cast<uint32_t>(
        _mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(p)), nlv)));
    while (m) {
      const char* nl = p + __builtin_ctz(m);
      on_line(t, line, nl, file_end, fill, lats);
      line = nl + 1;
      m &= m - 1;
    }
    p += 32;
  }
  while (line < e) {
    const char* nl = line;
    while (nl < e && *nl != '\n') ++nl;
    if (nl >= e) break;
    on_line(t, line, nl, file_end, fill, lats);
    line = nl + 1;
  }
}

void parse_range(Loc& t, const char* base, size_t begin, size_t end, size_t flen, bool fill, uint32_t* lats) {
  const char* file_end = base + flen;
  size_t pos = begin;
  while (pos < end) {
    size_t to = std::min(end, pos + (1u << 20));
    if (to < end) {
      while (to < end && base[to - 1] != '\n') ++to;
    }
    advise(base, pos, to, MADV_POPULATE_READ);
    parse_window(t, base + pos, base + to, file_end, fill, lats);
    advise(base, pos, to, MADV_DONTNEED);
    pos = to;
  }
}

void isort(uint32_t* a, uint32_t n) {
  for (uint32_t i = 1; i < n; ++i) {
    uint32_t v = a[i];
    uint32_t j = i;
    while (j && a[j - 1] > v) {
      a[j] = a[j - 1];
      --j;
    }
    a[j] = v;
  }
}

void select3(uint32_t* a, uint32_t n, uint32_t& p50, uint32_t& p95, uint32_t& p99) {
  uint32_t i50 = static_cast<uint32_t>((50ull * n + 99) / 100 - 1);
  uint32_t i95 = static_cast<uint32_t>((95ull * n + 99) / 100 - 1);
  uint32_t i99 = static_cast<uint32_t>((99ull * n + 99) / 100 - 1);
  if (n <= 64) {
    isort(a, n);
  } else {
    std::nth_element(a, a + i99, a + n);
    std::nth_element(a, a + i95, a + i99 + 1);
    std::nth_element(a, a + i50, a + i95 + 1);
  }
  p50 = a[i50];
  p95 = a[i95];
  p99 = a[i99];
}

char* fmt_u32(char* w, uint32_t x) {
  char tmp[10];
  int n = 0;
  do {
    tmp[n++] = static_cast<char>('0' + x % 10);
    x /= 10;
  } while (x);
  while (n--) *w++ = tmp[n];
  return w;
}

void init_hours(char out[kHours][21]) {
  for (int i = 0; i < kHours; ++i) {
    int total = i + 18;
    int day = total / 24;
    int hour = total % 24;
    int month = 10;
    int dom = 30;
    if (day == 1) dom = 31;
    else if (day == 2) {
      month = 11;
      dom = 1;
    } else if (day == 3) {
      month = 11;
      dom = 2;
    }
    std::snprintf(out[i], 21, "%04d-%02d-%02dT%02d:00:00Z", 2026, month, dom, hour);
  }
}

const char* env_or(const char* key, const char* fallback) {
  const char* v = std::getenv(key);
  return v && *v ? v : fallback;
}

void write_all(int fd, const char* p, size_t n) {
  while (n) {
    ssize_t k = ::write(fd, p, n);
    if (k < 0) die("write");
    p += k;
    n -= static_cast<size_t>(k);
  }
}

struct Buf {
  int fd = -1;
  char b[1 << 16];
  size_t n = 0;
  void put(const char* s, size_t k) {
    if (n + k > sizeof b) flush();
    if (k > sizeof b) {
      write_all(fd, s, k);
      return;
    }
    std::memcpy(b + n, s, k);
    n += k;
  }
  void flush() {
    if (n) write_all(fd, b, n);
    n = 0;
  }
};

}  // namespace

int main() {
  using Clock = std::chrono::steady_clock;
  auto t0 = Clock::now();
  auto ms = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); };
  bool stats = std::getenv("SD_STATS") != nullptr;

  std::string in = std::string(env_or("DATA", "/data")) + "/requests.log";
  std::string outp = std::string(env_or("OUT", "/out")) + "/summary.txt";
  Mapped file(in);
  int fd = ::open(outp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) die("open out");
  if (!file.len) {
    close(fd);
    return 0;
  }

  int T = worker_count();
  std::vector<size_t> cuts(static_cast<size_t>(T) + 1);
  cuts[0] = 0;
  cuts[static_cast<size_t>(T)] = file.len;
  for (int t = 1; t < T; ++t) {
    size_t pos = file.len * static_cast<size_t>(t) / static_cast<size_t>(T);
    if (pos) {
      while (pos < file.len && file.data[pos - 1] != '\n') ++pos;
    }
    cuts[static_cast<size_t>(t)] = pos;
  }

  std::vector<Loc> locals(static_cast<size_t>(T));
  {
    std::vector<std::thread> pool;
    for (int t = 0; t < T; ++t) {
      pool.emplace_back([&, t] {
        if (cuts[static_cast<size_t>(t)] < cuts[static_cast<size_t>(t) + 1])
          parse_range(locals[static_cast<size_t>(t)], file.data, cuts[static_cast<size_t>(t)],
                      cuts[static_cast<size_t>(t) + 1], file.len, false, nullptr);
      });
    }
    for (auto& th : pool) th.join();
  }
  if (stats) std::fprintf(stderr, "pass1 %.2f ms\n", ms());

  Loc global(false);
  std::vector<std::vector<uint32_t>> gid_of(static_cast<size_t>(T));
  for (int t = 0; t < T; ++t) {
    gid_of[static_cast<size_t>(t)].assign(locals[static_cast<size_t>(t)].nids, 0);
    for (const Slot& sl : locals[static_cast<size_t>(t)].slots) {
      if (!sl.h) continue;
      gid_of[static_cast<size_t>(t)][sl.id] =
          global.intern(locals[static_cast<size_t>(t)].bytes.data() + sl.off, sl.len, sl.h, true);
    }
  }
  const uint32_t nids = global.nids;
  std::vector<G> g(static_cast<size_t>(kHours) * nids);
  for (int t = 0; t < T; ++t) {
    for (uint32_t lid = 0; lid < locals[static_cast<size_t>(t)].nids; ++lid) {
      uint32_t gid = gid_of[static_cast<size_t>(t)][lid];
      for (int hour = 0; hour < kHours; ++hour) {
        G& dst = g[static_cast<size_t>(hour) * nids + gid];
        dst.n += locals[static_cast<size_t>(t)].n[static_cast<size_t>(hour) * kCap + lid];
        dst.err += locals[static_cast<size_t>(t)].e[static_cast<size_t>(hour) * kCap + lid];
      }
    }
  }
  uint32_t total = 0;
  for (G& dst : g) {
    dst.off = total;
    total += dst.n;
  }
  std::vector<uint32_t> run(g.size());
  for (size_t i = 0; i < g.size(); ++i) run[i] = g[i].off;
  for (int t = 0; t < T; ++t) {
    for (uint32_t lid = 0; lid < locals[static_cast<size_t>(t)].nids; ++lid) {
      uint32_t gid = gid_of[static_cast<size_t>(t)][lid];
      for (int hour = 0; hour < kHours; ++hour) {
        size_t li = static_cast<size_t>(hour) * kCap + lid;
        uint32_t c = locals[static_cast<size_t>(t)].n[li];
        size_t idx = static_cast<size_t>(hour) * nids + gid;
        locals[static_cast<size_t>(t)].n[li] = run[idx];
        run[idx] += c;
      }
    }
    std::vector<uint32_t>().swap(locals[static_cast<size_t>(t)].e);
  }
  run.clear();
  uint32_t* lats = static_cast<uint32_t*>(std::malloc(sizeof(uint32_t) * (total ? total : 1)));
  if (!lats) die("oom");
  if (stats) std::fprintf(stderr, "merge %.2f ms eps %u lats %u\n", ms(), nids, total);

  {
    std::vector<std::thread> pool;
    for (int t = 0; t < T; ++t) {
      pool.emplace_back([&, t] {
        if (cuts[static_cast<size_t>(t)] < cuts[static_cast<size_t>(t) + 1])
          parse_range(locals[static_cast<size_t>(t)], file.data, cuts[static_cast<size_t>(t)],
                      cuts[static_cast<size_t>(t) + 1], file.len, true, lats);
      });
    }
    for (auto& th : pool) th.join();
  }
  if (stats) std::fprintf(stderr, "pass2 %.2f ms\n", ms());
  locals.clear();
  gid_of.clear();

  int pt = std::min(T, std::max(1, static_cast<int>(g.size())));
  std::vector<std::thread> pct;
  for (int t = 0; t < pt; ++t) {
    pct.emplace_back([&, t] {
      for (size_t i = static_cast<size_t>(t); i < g.size(); i += static_cast<size_t>(pt)) {
        if (!g[i].n) continue;
        select3(lats + g[i].off, g[i].n, g[i].p50, g[i].p95, g[i].p99);
      }
    });
  }
  for (auto& th : pct) th.join();
  if (stats) std::fprintf(stderr, "pct %.2f ms\n", ms());
  madvise(lats, sizeof(uint32_t) * (total ? total : 1), MADV_DONTNEED);
  std::free(lats);
  malloc_trim(0);

  std::vector<uint32_t> eoff(nids), elen(nids);
  for (const Slot& sl : global.slots) {
    if (!sl.h) continue;
    eoff[sl.id] = sl.off;
    elen[sl.id] = sl.len;
  }
  std::vector<uint32_t> order(nids);
  for (uint32_t i = 0; i < nids; ++i) order[i] = i;
  const char* blob = global.bytes.data();
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    uint32_t na = elen[a], nb = elen[b];
    int c = std::memcmp(blob + eoff[a], blob + eoff[b], std::min(na, nb));
    if (c) return c < 0;
    return na < nb;
  });

  char hours[kHours][21];
  init_hours(hours);
  Buf out;
  out.fd = fd;
  for (int hour = 0; hour < kHours; ++hour) {
    for (uint32_t id : order) {
      const G& r = g[static_cast<size_t>(hour) * nids + id];
      if (!r.n) continue;
      char line[256];
      char* w = line;
      std::memcpy(w, hours[hour], 20);
      w += 20;
      *w++ = ' ';
      std::memcpy(w, blob + eoff[id], elen[id]);
      w += elen[id];
      *w++ = ' ';
      w = fmt_u32(w, r.n);
      *w++ = ' ';
      w = fmt_u32(w, r.p50);
      *w++ = ' ';
      w = fmt_u32(w, r.p95);
      *w++ = ' ';
      w = fmt_u32(w, r.p99);
      *w++ = ' ';
      w = fmt_u32(w, r.err);
      *w++ = ' ';
      w = fmt_u32(w, static_cast<uint32_t>(static_cast<uint64_t>(r.err) * 1000000ull / r.n));
      *w++ = '\n';
      out.put(line, static_cast<size_t>(w - line));
    }
  }
  out.flush();
  close(fd);
  if (stats) std::fprintf(stderr, "out %.2f ms\n", ms());
  return 0;
}
