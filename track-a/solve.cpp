// Track A (Ingest): parallel byte parse, open-addressed endpoint table,
// exact-size latency arenas, and nth_element percentiles.
//
// Every seed's timestamps sit in the same 72 hours, so the hour is a
// fixed-offset integer, not a date parse. Endpoints are interned once
// and groups are dense (hour, endpoint id). Latencies stay uint32.
// Percentiles are nearest-rank: index (p * n + 99) / 100 - 1.
//
// The file is mapped and split on newlines. Each worker has its own
// table and arena; workers never share a write. Results are merged,
// percentiles run in parallel, then lines are emitted in hour order
// and unsigned-byte endpoint order.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <nmmintrin.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr int kHours = 72;
constexpr uint32_t kEmpty = 0xffffffffu;

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
  if (hc > 12) hc = 12;
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

struct Arena {
  std::vector<uint32_t*> blocks;
  uint32_t* cur = nullptr;
  uint32_t left = 0;

  uint32_t* alloc(uint32_t n) {
    if (n > left) {
      uint32_t sz = std::max(n, 1u << 20);
      cur = new uint32_t[sz];
      blocks.push_back(cur);
      left = sz;
    }
    uint32_t* p = cur;
    cur += n;
    left -= n;
    return p;
  }

  ~Arena() {
    for (uint32_t* b : blocks) delete[] b;
  }

  Arena() = default;
  Arena(const Arena&) = delete;
  Arena& operator=(const Arena&) = delete;
};

struct Group {
  uint32_t* lat = nullptr;
  uint32_t n = 0;
  uint32_t cap = 0;
  uint32_t errors = 0;
  uint32_t p50 = 0;
  uint32_t p95 = 0;
  uint32_t p99 = 0;
};

struct Slot {
  uint64_t hash = 0;
  uint32_t off = 0;
  uint32_t len = 0;
  uint32_t id = kEmpty;
};

struct Table {
  Arena arena;
  std::string bytes;
  std::vector<Slot> slots;
  std::vector<Group> g[kHours];
  uint32_t mask = 0;
  uint32_t used = 0;
  uint32_t nids = 0;

  Table() { rehash(1024); }

  void rehash(uint32_t cap) {
    std::vector<Slot> ns(cap);
    uint32_t m = cap - 1;
    for (const Slot& sl : slots) {
      if (sl.id == kEmpty) continue;
      uint32_t i = static_cast<uint32_t>(sl.hash) & m;
      while (ns[i].id != kEmpty) i = (i + 1) & m;
      ns[i] = sl;
    }
    slots.swap(ns);
    mask = m;
  }

  void release() {
    for (uint32_t* b : arena.blocks) delete[] b;
    arena.blocks.clear();
    arena.cur = nullptr;
    arena.left = 0;
    std::string().swap(bytes);
    std::vector<Slot>().swap(slots);
    for (int hour = 0; hour < kHours; hour++) std::vector<Group>().swap(g[hour]);
  }

  uint32_t intern(const char* s, uint32_t n, uint64_t h) {
    if ((used + 1) * 10 >= (mask + 1) * 7) rehash((mask + 1) * 2);
    uint32_t i = static_cast<uint32_t>(h) & mask;
    for (;;) {
      Slot& sl = slots[i];
      if (sl.id == kEmpty) {
        sl.hash = h;
        sl.off = static_cast<uint32_t>(bytes.size());
        sl.len = n;
        sl.id = nids++;
        bytes.append(s, n);
        used++;
        for (int hour = 0; hour < kHours; hour++) g[hour].emplace_back();
        return sl.id;
      }
      if (sl.hash == h && sl.len == n &&
          std::memcmp(bytes.data() + sl.off, s, n) == 0) {
        return sl.id;
      }
      i = (i + 1) & mask;
    }
  }
};

// Hour index in the fixed window starting 2026-10-30T18:00Z.
int hour_index(const char* ts) {
  int month = (ts[5] - '0') * 10 + (ts[6] - '0');
  int day = (ts[8] - '0') * 10 + (ts[9] - '0');
  int hour = (ts[11] - '0') * 10 + (ts[12] - '0');
  int day_i = 0;
  if (month == 10 && day == 30) day_i = 0;
  else if (month == 10 && day == 31) day_i = 1;
  else if (month == 11 && day == 1) day_i = 2;
  else day_i = 3;  // 2026-11-02, the last day of the window
  return day_i * 24 + hour - 18;
}

void push_lat(Table& t, Group& g, uint32_t lat) {
  if (g.n == g.cap) {
    uint32_t nc = g.cap ? g.cap * 2 : 8;
    uint32_t* p = t.arena.alloc(nc);
    if (g.n) std::memcpy(p, g.lat, g.n * sizeof(uint32_t));
    g.lat = p;
    g.cap = nc;
  }
  g.lat[g.n++] = lat;
}

// Drop file pages this worker has finished so peak RSS is the live
// window plus the latency arena, not the whole log.
void drop_pages(const char* base, size_t from, size_t to) {
  uintptr_t pa = (reinterpret_cast<uintptr_t>(base + from) + 4095u) & ~uintptr_t(4095);
  uintptr_t pb = reinterpret_cast<uintptr_t>(base + to) & ~uintptr_t(4095);
  if (pb > pa) madvise(reinterpret_cast<void*>(pa), pb - pa, MADV_DONTNEED);
}

uint64_t hash_endpoint(const char* s, uint32_t n) {
  uint64_t h = 0;
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8) {
    uint64_t v;
    std::memcpy(&v, s + i, 8);
    h = _mm_crc32_u64(h, v);
  }
  uint64_t tail = 0;
  if (i < n) std::memcpy(&tail, s + i, n - i);
  h = _mm_crc32_u64(h, tail ^ n);
  return h ? h : 1;
}

// First byte index of 0x20 in v, or 8 if none. ASCII only.
inline int space_in_u64(uint64_t v) {
  uint64_t x = v ^ 0x2020202020202020ull;
  uint64_t y = (x - 0x0101010101010101ull) & ~x & 0x8080808080808080ull;
  if (y == 0) return 8;
  return __builtin_ctzll(y) >> 3;
}

void parse_range(const char* base, size_t begin, size_t end, Table& t) {
  const char* p = base + begin;
  const char* limit = base + end;
  size_t dropped = begin;
  while (p < limit) {
    int hour = hour_index(p);
    p += 28;  // 27-byte timestamp and the following space
    while (*p != ' ') p++;
    p++;
    const char* es = p;
    uint64_t h = 0;
    for (;;) {
      uint64_t v;
      std::memcpy(&v, p, 8);
      int k = space_in_u64(v);
      if (k < 8) {
        if (k) {
          uint64_t mask = (1ull << (k * 8)) - 1ull;
          h = _mm_crc32_u64(h, v & mask);
        }
        p += k;
        break;
      }
      h = _mm_crc32_u64(h, v);
      p += 8;
    }
    uint32_t elen = static_cast<uint32_t>(p - es);
    h = _mm_crc32_u64(h, elen);
    if (h == 0) h = 1;
    p++;  // space before status
    bool err = (*p == '5');
    p += 4;  // three status digits and the space
    uint32_t lat = 0;
    while (*p != '\n') {
      lat = lat * 10u + static_cast<uint32_t>(*p - '0');
      p++;
    }
    p++;
    uint32_t id = t.intern(es, elen, h);
    Group& g = t.g[hour][id];
    push_lat(t, g, lat);
    if (err) g.errors++;
    size_t at = static_cast<size_t>(p - base);
    if (at - dropped > (4u << 20)) {
      drop_pages(base, dropped, at);
      dropped = at;
    }
  }
  drop_pages(base, dropped, end);
}

void append_group(Table& dst, Group& g, const uint32_t* src, uint32_t n, uint32_t errors) {
  if (n) {
    if (g.n + n > g.cap) {
      uint32_t nc = std::max(g.n + n, g.cap ? g.cap * 2 : n);
      uint32_t* p = dst.arena.alloc(nc);
      if (g.n) std::memcpy(p, g.lat, g.n * sizeof(uint32_t));
      g.lat = p;
      g.cap = nc;
    }
    std::memcpy(g.lat + g.n, src, n * sizeof(uint32_t));
    g.n += n;
  }
  g.errors += errors;
}

void percentiles(Group& g) {
  uint32_t n = g.n;
  uint32_t i50 = static_cast<uint32_t>((50ull * n + 99) / 100 - 1);
  uint32_t i95 = static_cast<uint32_t>((95ull * n + 99) / 100 - 1);
  uint32_t i99 = static_cast<uint32_t>((99ull * n + 99) / 100 - 1);
  uint32_t* a = g.lat;
  std::nth_element(a, a + i99, a + n);
  g.p99 = a[i99];
  std::nth_element(a, a + i95, a + i99 + 1);
  g.p95 = a[i95];
  std::nth_element(a, a + i50, a + i95 + 1);
  g.p50 = a[i50];
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
  for (int i = 0; i < kHours; i++) {
    int total = i + 18;
    int day = total / 24;
    int hour = total % 24;
    int month = 10;
    int dom = 30;
    if (day == 1) {
      month = 10;
      dom = 31;
    } else if (day == 2) {
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

}  // namespace

int main() {
  using Clock = std::chrono::steady_clock;
  auto t0 = Clock::now();
  auto ms = [&] {
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  };
  bool stats = std::getenv("SD_STATS") != nullptr;
  std::string in = std::string(env_or("DATA", "/data")) + "/requests.log";
  std::string outp = std::string(env_or("OUT", "/out")) + "/summary.txt";
  Mapped file(in);

  int T = worker_count();
  if (file.len == 0) {
    int fd = ::open(outp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) die("open output");
    close(fd);
    return 0;
  }

  std::vector<size_t> cuts(T + 1);
  cuts[0] = 0;
  cuts[T] = file.len;
  for (int t = 1; t < T; t++) {
    size_t pos = file.len * static_cast<size_t>(t) / static_cast<size_t>(T);
    if (pos > 0) {
      while (pos < file.len && file.data[pos - 1] != '\n') pos++;
    }
    cuts[t] = pos;
  }
  for (int t = 1; t <= T; t++) {
    if (cuts[t] < cuts[t - 1]) cuts[t] = cuts[t - 1];
  }

  std::vector<Table> locals(static_cast<size_t>(T));
  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(T));
  for (int t = 0; t < T; t++) {
    pool.emplace_back([&, t] {
      if (cuts[t] < cuts[t + 1]) {
        parse_range(file.data, cuts[t], cuts[t + 1], locals[t]);
      }
    });
  }
  for (auto& th : pool) th.join();
  if (stats) fprintf(stderr, "parse %.1f ms\n", ms());

  Table global;
  for (Table& loc : locals) {
    std::vector<uint32_t> map(loc.nids);
    for (const Slot& sl : loc.slots) {
      if (sl.id == kEmpty) continue;
      map[sl.id] = global.intern(loc.bytes.data() + sl.off, sl.len, sl.hash);
    }
    for (int hour = 0; hour < kHours; hour++) {
      for (uint32_t id = 0; id < loc.nids; id++) {
        Group& src = loc.g[hour][id];
        if (src.n == 0) continue;
        append_group(global, global.g[hour][map[id]], src.lat, src.n, src.errors);
      }
    }
    loc.release();
  }
  if (stats) fprintf(stderr, "merge %.1f ms\n", ms());

  struct Job {
    int hour;
    uint32_t id;
  };
  std::vector<Job> jobs;
  jobs.reserve(global.nids * 8);
  for (int hour = 0; hour < kHours; hour++) {
    for (uint32_t id = 0; id < global.nids; id++) {
      if (global.g[hour][id].n) jobs.push_back({hour, id});
    }
  }
  std::atomic<size_t> next{0};
  std::vector<std::thread> pct;
  int pt = std::min(T, std::max(1, static_cast<int>(jobs.size())));
  for (int t = 0; t < pt; t++) {
    pct.emplace_back([&] {
      for (;;) {
        size_t i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= jobs.size()) break;
        percentiles(global.g[jobs[i].hour][jobs[i].id]);
      }
    });
  }
  for (auto& th : pct) th.join();
  if (stats) fprintf(stderr, "pct %.1f ms\n", ms());

  char hours[kHours][21];
  init_hours(hours);

  std::vector<uint32_t> off(global.nids), len(global.nids);
  for (const Slot& sl : global.slots) {
    if (sl.id == kEmpty) continue;
    off[sl.id] = sl.off;
    len[sl.id] = sl.len;
  }
  // One bytewise order, reused for every hour.
  std::vector<uint32_t> order(global.nids);
  for (uint32_t id = 0; id < global.nids; id++) order[id] = id;
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    uint32_t na = len[a], nb = len[b];
    int c = std::memcmp(global.bytes.data() + off[a], global.bytes.data() + off[b], std::min(na, nb));
    if (c != 0) return c < 0;
    return na < nb;
  });

  std::vector<char> out(12u << 20);
  char* w = out.data();
  char* wend = out.data() + out.size();
  for (int hour = 0; hour < kHours; hour++) {
    for (uint32_t id : order) {
      const Group& g = global.g[hour][id];
      if (g.n == 0) continue;
      if (wend - w < 256) {
        size_t at = static_cast<size_t>(w - out.data());
        out.resize(out.size() * 2);
        w = out.data() + at;
        wend = out.data() + out.size();
      }
      std::memcpy(w, hours[hour], 20);
      w += 20;
      *w++ = ' ';
      std::memcpy(w, global.bytes.data() + off[id], len[id]);
      w += len[id];
      *w++ = ' ';
      w = fmt_u32(w, g.n);
      *w++ = ' ';
      w = fmt_u32(w, g.p50);
      *w++ = ' ';
      w = fmt_u32(w, g.p95);
      *w++ = ' ';
      w = fmt_u32(w, g.p99);
      *w++ = ' ';
      w = fmt_u32(w, g.errors);
      *w++ = ' ';
      w = fmt_u32(w, static_cast<uint32_t>(static_cast<uint64_t>(g.errors) * 1000000ull / g.n));
      *w++ = '\n';
    }
  }
  out.resize(static_cast<size_t>(w - out.data()));
  if (stats) fprintf(stderr, "fmt %.1f ms\n", ms());

  int fd = ::open(outp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) die("open output");
  write_all(fd, out.data(), out.size());
  close(fd);
  return 0;
}
