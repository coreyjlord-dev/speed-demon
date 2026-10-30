// Track A: one SIMD pass, records filed per hour, in-cache select, writer thread.
// Nearest rank: (p * n + 99) / 100 - 1. A 5xx is status[0] == '5'.

#include <algorithm>
#include <atomic>
#include <cstring>
#include <charconv>
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
#include <vector>

namespace {

constexpr int kHours = 72;
constexpr uint32_t kCap = 2048;
constexpr uint32_t kSlots = 4096;
constexpr uint32_t kBlock = 2048;

[[noreturn]] void die(const char* m) {
  std::fprintf(stderr, "track-a: %s\n", m);
  std::exit(1);
}

void tune() {
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
  mallopt(M_TRIM_THRESHOLD, 128 * 1024);
  mallopt(M_MMAP_THRESHOLD, 256 * 1024);
  mallopt(M_TOP_PAD, 0);
}

int nthreads() {
  if (const char* e = std::getenv("SD_THREADS")) {
    int n = std::atoi(e);
    if (n >= 1 && n <= 64) return n;
  }
  unsigned h = std::thread::hardware_concurrency();
  if (!h) return 1;
  if (h >= 8) return 6;
  return (int)h;
}

struct Mapped {
  int fd = -1;
  size_t len = 0;
  const char* p = nullptr;
  explicit Mapped(const char* path) {
    fd = ::open(path, O_RDONLY);
    if (fd < 0) die("open");
    struct stat st {};
    if (fstat(fd, &st)) die("stat");
    len = (size_t)st.st_size;
    if (!len) return;
    void* m = ::mmap(nullptr, len, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) die("mmap");
    p = (const char*)m;
    madvise((void*)p, len, MADV_RANDOM);
  }
  ~Mapped() {
    if (p) munmap((void*)p, len);
    if (fd >= 0) close(fd);
  }
};

void drop_pages(const char* base, size_t a, size_t b) {
  uintptr_t x = ((uintptr_t)(base + a) + 4095) & ~(uintptr_t)4095;
  uintptr_t y = (uintptr_t)(base + b) & ~(uintptr_t)4095;
  if (y > x) madvise((void*)x, y - x, MADV_DONTNEED);
}

struct Rec {
  uint32_t lat;
  uint16_t id;
  uint16_t err;
};

struct Block {
  Block* next;
  uint32_t n;
  Rec r[kBlock];
};

struct Hour {
  Block* head = nullptr;
  Block* tail = nullptr;
  void add(Rec x) {
    if (!tail || tail->n == kBlock) {
      auto* b = (Block*)std::malloc(sizeof(Block));
      if (!b) die("oom");
      b->next = nullptr;
      b->n = 0;
      if (tail) tail->next = b;
      else head = b;
      tail = b;
    }
    tail->r[tail->n++] = x;
  }
  void release() {
    for (Block* b = head; b;) {
      Block* n = b->next;
      std::free(b);
      b = n;
    }
    head = tail = nullptr;
  }
};

struct Slot {
  uint64_t h = 0;
  uint32_t off = 0, len = 0, id = 0;
};

struct Worker {
  std::string bytes;
  std::vector<Slot> slots;
  uint32_t mask = kSlots - 1, nids = 0;
  const char* blob = nullptr;
  Hour hour[kHours];
  Worker() {
    bytes.reserve(1 << 16);
    slots.resize(kSlots);
    blob = bytes.data();
  }
  uint32_t intern(const char* s, uint32_t n, uint64_t h) {
    uint32_t i = (uint32_t)h & mask;
    for (;;) {
      Slot& sl = slots[i];
      if (sl.h == h && sl.len == n) {
        if (std::memcmp(blob + sl.off, s, n) == 0) return sl.id;
      } else if (!sl.h) {
        if (nids >= kCap) die("endpoints");
        sl.h = h;
        sl.off = (uint32_t)bytes.size();
        sl.len = n;
        sl.id = nids++;
        bytes.append(s, n);
        blob = bytes.data();
        return sl.id;
      }
      i = (i + 1) & mask;
    }
  }
};

inline int hour_of(const char* t) {
  int month = t[6] - '0';
  int day = (t[8] - '0') * 10 + (t[9] - '0');
  int hr = (t[11] - '0') * 10 + (t[12] - '0');
  return (month ? day + 1 : day - 30) * 24 + hr - 18;
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
  h ^= (uint64_t)n * 0x9E3779B185EBCA87ull;
  h *= 0x100000001B3ull;
  return h ? h : 1;
}

inline uint32_t parse_lat(const char* d, int k, const char* lim) {
  if (k <= 8 && d + 8 <= lim) {
    uint64_t v;
    std::memcpy(&v, d, 8);
    uint64_t mask = k == 8 ? ~0ull : (1ull << (k * 8)) - 1ull;
    v &= mask & 0x0F0F0F0F0F0F0F0Full;
    uint32_t x = v & 255u;
    x = x * 10u + ((v >> 8) & 255u);
    x = x * 10u + ((v >> 16) & 255u);
    x = x * 10u + ((v >> 24) & 255u);
    x = x * 10u + (uint32_t)((v >> 32) & 255u);
    x = x * 10u + (uint32_t)((v >> 40) & 255u);
    x = x * 10u + (uint32_t)((v >> 48) & 255u);
    x = x * 10u + (uint32_t)(v >> 56);
    static const uint32_t div10[9] = {100000000u, 10000000u, 1000000u, 100000u, 10000u,
                                       1000u,      100u,      10u,      1u};
    return x / div10[k];
  }
  uint32_t x = 0;
  for (int i = 0; i < k; ++i) x = x * 10u + (uint32_t)(d[i] - '0');
  return x;
}

// Spaces and the newline in the 64 bytes at `m`. s0, s1, s2 are the first three spaces.
inline bool scan64(const char* m, const char* lim, int& s0, int& s1, int& s2, int& npos) {
  if (m + 64 > lim) return false;
  __m256i a = _mm256_loadu_si256((const __m256i*)m);
  __m256i b = _mm256_loadu_si256((const __m256i*)(m + 32));
  __m256i sp = _mm256_set1_epi8(' ');
  __m256i nl = _mm256_set1_epi8('\n');
  uint64_t ms = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, sp));
  ms |= (uint64_t)(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(b, sp)) << 32;
  uint64_t mn = (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(a, nl));
  mn |= (uint64_t)(uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(b, nl)) << 32;
  if (!mn || !ms) return false;
  npos = __builtin_ctzll(mn);
  s0 = __builtin_ctzll(ms);
  if (s0 >= npos) return false;
  ms &= ms - 1;
  if (!ms) return false;
  s1 = __builtin_ctzll(ms);
  if (s1 >= npos) return false;
  ms &= ms - 1;
  if (!ms) return false;
  s2 = __builtin_ctzll(ms);
  return s2 < npos;
}

inline void take(Worker& w, const char* line, const char* nl, const char* lim) {
  const char* m = line + 28;
  int s0, s1, s2, npos;
  const char* ep;
  uint32_t elen, err, lat;
  if (scan64(m, lim, s0, s1, s2, npos)) {
    ep = m + s0 + 1;
    elen = (uint32_t)(s1 - s0 - 1);
    err = m[s1 + 1] == '5';
    lat = parse_lat(m + s2 + 1, npos - s2 - 1, lim);
  } else {
    const char* p = m;
    while (*p != ' ') ++p;
    ep = p + 1;
    const char* ee = ep;
    while (*ee != ' ') ++ee;
    elen = (uint32_t)(ee - ep);
    err = ee[1] == '5';
    const char* d = ee + 5;
    lat = parse_lat(d, (int)(nl - d), lim);
  }
  int hr = hour_of(line);
  uint32_t id = w.intern(ep, elen, hash_ep(ep, elen));
  w.hour[hr].add(Rec{lat, (uint16_t)id, (uint16_t)err});
}

void parse_slice(Worker& w, const char* base, size_t begin, size_t end, size_t flen) {
  const char* lim = base + flen;
  const char* line = base + begin;
  const char* e = base + end;
  const char* p = line;
  const __m256i nlv = _mm256_set1_epi8('\n');
  size_t dropped = begin;
  while (p + 32 <= e) {
    uint32_t m =
        (uint32_t)_mm256_movemask_epi8(_mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i*)p), nlv));
    while (m) {
      const char* nl = p + __builtin_ctz(m);
      take(w, line, nl, lim);
      line = nl + 1;
      m &= m - 1;
    }
    p += 32;
    size_t at = (size_t)(line - base);
    if (at > dropped + (1u << 20)) {
      drop_pages(base, dropped, at - 4096);
      dropped = at - 4096;
    }
  }
  while (line < e) {
    const char* nl = line;
    while (nl < e && *nl != '\n') ++nl;
    if (nl >= e) break;
    take(w, line, nl, lim);
    line = nl + 1;
  }
  drop_pages(base, dropped, end);
}

void isort(uint32_t* a, uint32_t n) {
  for (uint32_t i = 1; i < n; ++i) {
    uint32_t v = a[i], j = i;
    while (j && a[j - 1] > v) {
      a[j] = a[j - 1];
      --j;
    }
    a[j] = v;
  }
}

void select3(uint32_t* a, uint32_t n, uint32_t& p50, uint32_t& p95, uint32_t& p99) {
  uint32_t i50 = (uint32_t)((50ull * n + 99) / 100 - 1);
  uint32_t i95 = (uint32_t)((95ull * n + 99) / 100 - 1);
  uint32_t i99 = (uint32_t)((99ull * n + 99) / 100 - 1);
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

void put_u32(char*& w, uint32_t x) {
  auto r = std::to_chars(w, w + 12, x);
  w = r.ptr;
}

struct Piece {
  std::string text;
  std::atomic<int> ready{0};
};

}  // namespace

int main() {
  tune();
  const char* data = std::getenv("DATA");
  const char* outd = std::getenv("OUT");
  std::string in = std::string(data && *data ? data : "/data") + "/requests.log";
  std::string outp = std::string(outd && *outd ? outd : "/out") + "/summary.txt";
  Mapped file(in.c_str());
  FILE* fp = std::fopen(outp.c_str(), "wb");
  if (!fp) die("out");
  if (!file.len) {
    std::fclose(fp);
    return 0;
  }
  int T = nthreads();
  std::vector<size_t> cut(T + 1);
  cut[0] = 0;
  cut[T] = file.len;
  for (int t = 1; t < T; ++t) {
    size_t pos = file.len * (size_t)t / (size_t)T;
    if (pos)
      while (pos < file.len && file.p[pos - 1] != '\n') ++pos;
    cut[t] = pos;
  }
  std::vector<Worker> ws(T);
  {
    std::vector<std::thread> pool;
    for (int t = 0; t < T; ++t)
      pool.emplace_back([&, t] {
        if (cut[t] < cut[t + 1]) parse_slice(ws[t], file.p, cut[t], cut[t + 1], file.len);
      });
    for (auto& th : pool) th.join();
  }

  struct Name {
    const char* s;
    uint32_t n;
    uint16_t tid, lid;
  };
  std::vector<Name> names;
  for (int t = 0; t < T; ++t)
    for (const Slot& sl : ws[t].slots)
      if (sl.h) names.push_back({ws[t].bytes.data() + sl.off, sl.len, (uint16_t)t, (uint16_t)sl.id});
  std::sort(names.begin(), names.end(), [](const Name& a, const Name& b) {
    int c = std::memcmp(a.s, b.s, std::min(a.n, b.n));
    return c ? c < 0 : a.n < b.n;
  });
  std::string blob;
  blob.reserve(1 << 16);
  std::vector<uint32_t> eoff, elen;
  std::vector<std::vector<uint32_t>> rank(T);
  for (int t = 0; t < T; ++t) rank[t].assign(kCap, 0);
  uint32_t nids = 0;
  for (size_t i = 0; i < names.size(); ++i) {
    bool same = i && names[i].n == names[i - 1].n &&
                std::memcmp(names[i].s, names[i - 1].s, names[i].n) == 0;
    if (!same) {
      eoff.push_back((uint32_t)blob.size());
      elen.push_back(names[i].n);
      blob.append(names[i].s, names[i].n);
      ++nids;
    }
    rank[names[i].tid][names[i].lid] = nids - 1;
  }

  char hours[kHours][21];
  for (int i = 0; i < kHours; ++i) {
    int total = i + 18, day = total / 24, hr = total % 24, month = 10, dom = 30;
    if (day == 1) dom = 31;
    else if (day == 2) {
      month = 11;
      dom = 1;
    } else if (day == 3) {
      month = 11;
      dom = 2;
    }
    std::snprintf(hours[i], 21, "%04d-%02d-%02dT%02d:00:00Z", 2026, month, dom, hr);
  }

  Piece piece[kHours];
  std::thread writer([&] {
    for (int h = 0; h < kHours; ++h) {
      while (!piece[h].ready.load(std::memory_order_acquire)) _mm_pause();
      if (!piece[h].text.empty())
        std::fwrite(piece[h].text.data(), 1, piece[h].text.size(), fp);
      std::string().swap(piece[h].text);
    }
  });

  std::atomic<int> next{0};
  std::vector<std::thread> hw;
  for (int t = 0; t < T; ++t) {
    hw.emplace_back([&] {
      for (;;) {
        int h = next.fetch_add(1, std::memory_order_relaxed);
        if (h >= kHours) break;
        uint32_t cnt[kCap] = {};
        uint32_t er[kCap] = {};
        uint32_t N = 0;
        for (int w = 0; w < T; ++w) {
          for (Block* b = ws[w].hour[h].head; b; b = b->next) {
            for (uint32_t i = 0; i < b->n; ++i) {
              uint32_t g = rank[w][b->r[i].id];
              cnt[g]++;
              er[g] += b->r[i].err;
              ++N;
            }
          }
        }
        std::string text;
        if (N) {
          uint32_t pref[kCap], cur[kCap], s = 0;
          for (uint32_t g = 0; g < nids; ++g) {
            pref[g] = s;
            s += cnt[g];
          }
          std::memcpy(cur, pref, nids * sizeof(uint32_t));
          std::vector<uint32_t> lat(N);
          for (int w = 0; w < T; ++w) {
            for (Block* b = ws[w].hour[h].head; b; b = b->next) {
              for (uint32_t i = 0; i < b->n; ++i)
                lat[cur[rank[w][b->r[i].id]]++] = b->r[i].lat;
            }
          }
          text.reserve(N);
          const char* bp = blob.data();
          for (uint32_t g = 0; g < nids; ++g) {
            if (!cnt[g]) continue;
            uint32_t p50, p95, p99;
            select3(lat.data() + pref[g], cnt[g], p50, p95, p99);
            char line[256];
            char* o = line;
            std::memcpy(o, hours[h], 20);
            o += 20;
            *o++ = ' ';
            std::memcpy(o, bp + eoff[g], elen[g]);
            o += elen[g];
            *o++ = ' ';
            put_u32(o, cnt[g]);
            *o++ = ' ';
            put_u32(o, p50);
            *o++ = ' ';
            put_u32(o, p95);
            *o++ = ' ';
            put_u32(o, p99);
            *o++ = ' ';
            put_u32(o, er[g]);
            *o++ = ' ';
            put_u32(o, (uint32_t)((uint64_t)er[g] * 1000000ull / cnt[g]));
            *o++ = '\n';
            text.append(line, (size_t)(o - line));
          }
        }
        for (int w = 0; w < T; ++w) ws[w].hour[h].release();
        piece[h].text = std::move(text);
        piece[h].ready.store(1, std::memory_order_release);
      }
    });
  }
  for (auto& th : hw) th.join();
  writer.join();
  std::fclose(fp);
  malloc_trim(0);
  return 0;
}
