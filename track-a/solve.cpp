// Track A: one tail scan, per-hour records, in-cache ranks, writer thread.
// Nearest rank is (p * n + 99) / 100 - 1. A 5xx is status[0] == '5'.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

constexpr int kH = 72;
constexpr uint32_t kCap = 2048;
constexpr uint32_t kSlots = 4096;

[[noreturn]] void die(const char* m) {
  std::fprintf(stderr, "track-a: %s\n", m);
  std::exit(1);
}

void tune() {
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
  mallopt(M_TRIM_THRESHOLD, 64 * 1024);
  mallopt(M_MMAP_THRESHOLD, 128 * 1024);
  mallopt(M_TOP_PAD, 0);
}

int nthreads() {
  if (const char* e = std::getenv("SD_THREADS")) {
    int n = std::atoi(e);
    if (n >= 1 && n <= 64) return n;
  }
  unsigned h = std::thread::hardware_concurrency();
  if (!h) return 1;
  return h > 12 ? 12 : (int)h;
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

#pragma pack(push, 1)
struct Rec {
  uint32_t lat;
  uint16_t meta;
};
#pragma pack(pop)
static_assert(sizeof(Rec) == 6, "rec");

struct B {
  B* next;
  uint32_t n;
  Rec r[1024];
};
struct Hour {
  B* head = nullptr;
  B* tail = nullptr;
  void add(uint32_t lat, uint16_t meta) {
    if (!tail || tail->n == 1024) {
      auto* b = (B*)std::malloc(sizeof(B));
      if (!b) die("oom");
      b->next = nullptr;
      b->n = 0;
      if (tail) tail->next = b;
      else head = b;
      tail = b;
    }
    tail->r[tail->n++] = Rec{lat, meta};
  }
  void clear() {
    for (B* b = head; b;) {
      B* n = b->next;
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
  Slot slots[kSlots]{};
  uint32_t nids = 0;
  const char* blob = nullptr;
  Hour hour[kH];
  Worker() {
    bytes.reserve(1 << 16);
    blob = bytes.data();
  }
  uint32_t intern(const char* s, uint32_t n, uint64_t h) {
    uint32_t i = (uint32_t)h & (kSlots - 1);
    for (;;) {
      Slot& sl = slots[i];
      if (sl.h == h && sl.len == n) {
        if (!std::memcmp(blob + sl.off, s, n)) return sl.id;
      } else if (!sl.h) {
        if (nids >= kCap) die("endpoints");
        sl = Slot{h, (uint32_t)bytes.size(), n, nids++};
        bytes.append(s, n);
        blob = bytes.data();
        return sl.id;
      }
      i = (i + 1) & (kSlots - 1);
    }
  }
};

inline int hour_of(const char* t) {
  int mo = t[6] - '0', day = (t[8] - '0') * 10 + (t[9] - '0');
  int hr = (t[11] - '0') * 10 + (t[12] - '0');
  return (mo ? day + 1 : day - 30) * 24 + hr - 18;
}

inline int mlen(const char* p) {
  unsigned c = (unsigned char)p[0];
  if (c == 'G') return 3;
  if (c == 'P') return p[1] == 'O' ? 4 : p[1] == 'A' ? 5 : 3;
  if (c == 'D') return 6;
  if (c == 'H') return 4;
  return 7;
}

alignas(32) static const uint8_t kI0[32] = {0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15,
                                  16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31};
alignas(32) static const uint8_t kI1[32] = {32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
                                  48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63};

inline uint64_t fold8(__m256i v) {
  __m128i lo = _mm256_castsi256_si128(v);
  __m128i hi = _mm256_extracti128_si256(v, 1);
  __m128i x = _mm_xor_si128(lo, hi);
  uint64_t a = (uint64_t)_mm_cvtsi128_si64(x);
  uint64_t b = (uint64_t)_mm_cvtsi128_si64(_mm_unpackhi_epi64(x, x));
  return a ^ b;
}

inline uint64_t hash_ep(const char* s, uint32_t n, const char* lim) {
  uint64_t h = 0;
  if (n <= 64 && s + 64 <= lim) {
    __m256i limv = _mm256_set1_epi8((char)n);
    __m256i a = _mm256_and_si256(_mm256_loadu_si256((const __m256i*)s),
                                 _mm256_cmpgt_epi8(limv, _mm256_load_si256((const __m256i*)kI0)));
    __m256i b = _mm256_and_si256(_mm256_loadu_si256((const __m256i*)(s + 32)),
                                 _mm256_cmpgt_epi8(limv, _mm256_load_si256((const __m256i*)kI1)));
    h = fold8(a) ^ fold8(b);
  } else {
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
  }
  h ^= (uint64_t)n * 0x9E3779B185EBCA87ull;
  h *= 0x100000001B3ull;
  return h ? h : 1;
}

inline uint32_t parse_lat(const char* d, int k, const char* lim) {
  if (k <= 8 && d + 8 <= lim) {
    uint64_t v;
    std::memcpy(&v, d, 8);
    v &= (k == 8 ? ~0ull : (1ull << (k * 8)) - 1ull) & 0x0F0F0F0F0F0F0F0Full;
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

inline void take(Worker& w, const char* line, const char* nl, const char* lim) {
  const char* win = nl - 16;
  __m128i v = _mm_loadu_si128((const __m128i*)win);
  uint32_t sm = (uint32_t)_mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_set1_epi8(' ')));
  const char* sp = win + (31 - __builtin_clz(sm));
  const char* m = line + 28;
  const char* ep = m + mlen(m) + 1;
  uint32_t elen = (uint32_t)((sp - 4) - ep);
  uint32_t err = sp[-3] == '5';
  int k = (int)(nl - (sp + 1));
  uint32_t lat = parse_lat(sp + 1, k, lim);
  uint32_t id = w.intern(ep, elen, hash_ep(ep, elen, lim));
  w.hour[hour_of(line)].add(lat, (uint16_t)(id | (err << 15)));
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
    (void)dropped;
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
    while (j && a[j - 1] > v) a[j] = a[j - 1], --j;
    a[j] = v;
  }
}

void select3(uint32_t* a, uint32_t n, uint32_t& a50, uint32_t& a95, uint32_t& a99) {
  uint32_t i50 = (uint32_t)((50ull * n + 99) / 100 - 1);
  uint32_t i95 = (uint32_t)((95ull * n + 99) / 100 - 1);
  uint32_t i99 = (uint32_t)((99ull * n + 99) / 100 - 1);
  if (n <= 48) isort(a, n);
  else {
    std::nth_element(a, a + i99, a + n);
    std::nth_element(a, a + i95, a + i99 + 1);
    std::nth_element(a, a + i50, a + i95 + 1);
  }
  a50 = a[i50];
  a95 = a[i95];
  a99 = a[i99];
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
    int c = std::memcmp(a.s, b.s, a.n < b.n ? a.n : b.n);
    return c ? c < 0 : a.n < b.n;
  });
  std::string blob;
  blob.reserve(1 << 16);
  std::vector<uint32_t> eoff, elen, rank(T * kCap);
  uint32_t nids = 0;
  for (size_t i = 0; i < names.size(); ++i) {
    bool same = i && names[i].n == names[i - 1].n && !std::memcmp(names[i].s, names[i - 1].s, names[i].n);
    if (!same) {
      eoff.push_back((uint32_t)blob.size());
      elen.push_back(names[i].n);
      blob.append(names[i].s, names[i].n);
      ++nids;
    }
    rank[names[i].tid * kCap + names[i].lid] = nids - 1;
  }

  char hours[kH][21];
  for (int i = 0; i < kH; ++i) {
    int tot = i + 18, day = tot / 24, hr = tot % 24, month = 10, dom = 30;
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

  Piece piece[kH];
  std::thread writer([&] {
    for (int h = 0; h < kH; ++h) {
      while (!piece[h].ready.load(std::memory_order_acquire)) _mm_pause();
      if (!piece[h].text.empty()) std::fwrite(piece[h].text.data(), 1, piece[h].text.size(), fp);
    }
  });

  std::atomic<int> next{0};
  std::vector<std::thread> hw;
  int ht = T < kH ? T : kH;
  for (int t = 0; t < ht; ++t) {
    hw.emplace_back([&] {
      for (;;) {
        int h = next.fetch_add(1, std::memory_order_relaxed);
        if (h >= kH) break;
        uint32_t cnt[kCap] = {}, er[kCap] = {}, N = 0;
        for (int w = 0; w < T; ++w) {
          for (B* b = ws[w].hour[h].head; b; b = b->next) {
            for (uint32_t i = 0; i < b->n; ++i) {
              uint32_t g = rank[w * kCap + (b->r[i].meta & 0x7fff)];
              cnt[g]++;
              er[g] += b->r[i].meta >> 15;
              ++N;
            }
          }
        }
        std::string text;
        if (N) {
          uint32_t pref[kCap], cur[kCap], s = 0;
          for (uint32_t g = 0; g < nids; ++g) {
            pref[g] = cur[g] = s;
            s += cnt[g];
          }
          std::vector<uint32_t> lat(N);
          for (int w = 0; w < T; ++w) {
            for (B* b = ws[w].hour[h].head; b; b = b->next)
              for (uint32_t i = 0; i < b->n; ++i)
                lat[cur[rank[w * kCap + (b->r[i].meta & 0x7fff)]]++] = b->r[i].lat;
          }
          text.reserve((size_t)N * 48);
          const char* bp = blob.data();
          for (uint32_t g = 0; g < nids; ++g) {
            if (!cnt[g]) continue;
            uint32_t p50, p95, p99;
            select3(lat.data() + pref[g], cnt[g], p50, p95, p99);
            char line[192];
            char* o = line;
            std::memcpy(o, hours[h], 20);
            o += 20;
            *o++ = ' ';
            std::memcpy(o, bp + eoff[g], elen[g]);
            o += elen[g];
            auto put = [&](uint32_t x) {
              char tmp[12];
              int n = 0;
              do tmp[n++] = (char)('0' + x % 10); while (x /= 10);
              while (n--) *o++ = tmp[n];
            };
            *o++ = ' ';
            put(cnt[g]);
            *o++ = ' ';
            put(p50);
            *o++ = ' ';
            put(p95);
            *o++ = ' ';
            put(p99);
            *o++ = ' ';
            put(er[g]);
            *o++ = ' ';
            put((uint32_t)((uint64_t)er[g] * 1000000ull / cnt[g]));
            *o++ = '\n';
            text.append(line, (size_t)(o - line));
          }
        }
        for (int w = 0; w < T; ++w) ws[w].hour[h].clear();
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
