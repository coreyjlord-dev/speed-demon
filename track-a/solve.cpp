// Track A: tail scan, per-hour 6-byte records, in-cache select, writer thread.
// Nearest rank is (p * n + 99) / 100 - 1. A 5xx is status[0] == '5'.

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
#include <vector>

namespace {

constexpr int kH = 72;
constexpr uint32_t kCap = 2048;
constexpr uint32_t kSlots = 4096;

[[noreturn]] void die(const char* m) {
  fprintf(stderr, "track-a: %s\n", m);
  exit(1);
}

void tune() {
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
  mallopt(M_TRIM_THRESHOLD, 64 * 1024);
  mallopt(M_MMAP_THRESHOLD, 128 * 1024);
  mallopt(M_TOP_PAD, 0);
}

int nthreads() {
  if (const char* e = getenv("SD_THREADS")) {
    int n = atoi(e);
    if (n >= 1 && n <= 64) return n;
  }
  unsigned h = std::thread::hardware_concurrency();
  return h ? (h > 12 ? 12 : (int)h) : 1;
}

void drop_pages(const char* base, size_t a, size_t b) {
  uintptr_t x = ((uintptr_t)(base + a) + 4095) & ~(uintptr_t)4095;
  uintptr_t y = (uintptr_t)(base + b) & ~(uintptr_t)4095;
  if (y > x) madvise((void*)x, y - x, MADV_DONTNEED);
}

struct __attribute__((packed)) Rec {
  uint32_t lat;
  uint16_t meta;
};
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
      auto* b = (B*)malloc(sizeof(B));
      if (!b) die("oom");
      b->next = nullptr;
      b->n = 0;
      (tail ? tail->next : head) = b;
      tail = b;
    }
    tail->r[tail->n++] = Rec{lat, meta};
  }
  void clear() {
    for (B* b = head; b;) {
      B* n = b->next;
      free(b);
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
  uint32_t intern(const char* s, uint32_t n, uint64_t h) {
    uint32_t i = (uint32_t)h & (kSlots - 1);
    for (;;) {
      Slot& sl = slots[i];
      if (sl.h == h && sl.len == n && !memcmp(blob + sl.off, s, n)) return sl.id;
      if (!sl.h) {
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

inline int space_at(uint64_t v) {
  uint64_t x = v ^ 0x2020202020202020ull;
  uint64_t y = (x - 0x0101010101010101ull) & ~x & 0x8080808080808080ull;
  return __builtin_ctzll(y) >> 3;
}

// First, middle, and last 8 bytes. memcmp resolves the rare collision.
inline uint64_t hash_ep(const char* s, uint32_t n) {
  uint64_t a = 0, b = 0, m = 0;
  memcpy(&a, s, 8);
  if (n < 8) a &= (1ull << (n * 8)) - 1ull;
  if (n > 8) {
    memcpy(&b, s + n - 8, 8);
    memcpy(&m, s + (n / 2) - 4, 8);
  }
  uint64_t h = _mm_crc32_u64(_mm_crc32_u64(_mm_crc32_u64(n, a), m), b);
  return h ? h : 1;
}

struct Div {
  uint64_t m;
  unsigned s;
};
// x / 10^(8-k) for the zero-padded 8-byte horner. Checked for every multiple.
static constexpr Div kDiv[9] = {
    {90071993ull, 53}, {112589991ull, 50}, {140737489ull, 47}, {87960931ull, 43},
    {109951163ull, 40}, {68719477ull, 36},  {42949673ull, 32},  {429496730ull, 32},
    {4294967296ull, 32}};

inline uint32_t parse_lat(const char* d, int k, const char* lim) {
  if (k <= 8 && d + 8 <= lim) {
    uint64_t v;
    memcpy(&v, d, 8);
    v &= (k == 8 ? ~0ull : (1ull << (k * 8)) - 1ull) & 0x0F0F0F0F0F0F0F0Full;
    uint32_t x = 0;
    for (int i = 0; i < 8; ++i) x = x * 10u + (uint32_t)((v >> (8 * i)) & 255u);
    return (uint32_t)((x * kDiv[k].m) >> kDiv[k].s);
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
  uint64_t mw;
  memcpy(&mw, line + 28, 8);
  const char* ep = line + 29 + space_at(mw);
  uint32_t elen = (uint32_t)(sp - 4 - ep);
  int kd = (int)(nl - sp - 1);
  uint32_t id = w.intern(ep, elen, hash_ep(ep, elen));
  uint16_t meta = (uint16_t)(id | ((sp[-3] == '5') << 15));
  w.hour[hour_of(line)].add(parse_lat(sp + 1, kd, lim), meta);
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

inline char* putu(char* o, uint32_t x) {
  *o++ = ' ';
  char t[10];
  int n = 0;
  do t[n++] = (char)('0' + x % 10);
  while (x /= 10);
  while (n--) *o++ = t[n];
  return o;
}

struct Piece {
  std::string text;
  std::atomic<int> ready{0};
};

template <class F>
void run(int n, F fn) {
  std::vector<std::thread> pool;
  for (int t = 0; t < n; ++t) pool.emplace_back(fn, t);
  for (auto& th : pool) th.join();
}

}  // namespace

int main() {
  tune();
  const char* data = getenv("DATA");
  const char* outd = getenv("OUT");
  std::string in = std::string(data && *data ? data : "/data") + "/requests.log";
  std::string outp = std::string(outd && *outd ? outd : "/out") + "/summary.txt";
  int fd = open(in.c_str(), O_RDONLY);
  if (fd < 0) die("open");
  struct stat st {};
  if (fstat(fd, &st)) die("stat");
  size_t flen = (size_t)st.st_size;
  FILE* fp = fopen(outp.c_str(), "wb");
  if (!fp) die("out");
  if (!flen) {
    fclose(fp);
    return 0;
  }
  void* mapped = mmap(nullptr, flen, PROT_READ, MAP_PRIVATE, fd, 0);
  if (mapped == MAP_FAILED) die("mmap");
  madvise(mapped, flen, MADV_RANDOM);
  const char* file = (const char*)mapped;
  int T = nthreads();
  std::vector<size_t> cut(T + 1);
  cut[0] = 0;
  cut[T] = flen;
  for (int t = 1; t < T; ++t) {
    size_t pos = flen * (size_t)t / (size_t)T;
    if (pos)
      while (pos < flen && file[pos - 1] != '\n') ++pos;
    cut[t] = pos;
  }
  std::vector<Worker> ws(T);
  for (int t = 0; t < T; ++t) ws[t].bytes.reserve(1 << 16);
  run(T, [&](int t) {
    if (cut[t] < cut[t + 1]) parse_slice(ws[t], file, cut[t], cut[t + 1], flen);
  });
  drop_pages(file, 0, flen);

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
    int c = memcmp(a.s, b.s, a.n < b.n ? a.n : b.n);
    return c ? c < 0 : a.n < b.n;
  });
  std::string blob;
  blob.reserve(1 << 16);
  std::vector<uint32_t> eoff, elen, rank(T * kCap);
  uint32_t nids = 0;
  for (size_t i = 0; i < names.size(); ++i) {
    bool same = i && names[i].n == names[i - 1].n && !memcmp(names[i].s, names[i - 1].s, names[i].n);
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
    int tot = i + 18, day = tot / 24, hr = tot % 24;
    int month = day < 2 ? 10 : 11;
    int dom = day == 0 ? 30 : day == 1 ? 31 : day - 1;
    snprintf(hours[i], 21, "%04d-%02d-%02dT%02d:00:00Z", 2026, month, dom, hr);
  }

  Piece piece[kH];
  std::thread writer([&] {
    for (int h = 0; h < kH; ++h) {
      while (!piece[h].ready.load(std::memory_order_acquire)) _mm_pause();
      if (!piece[h].text.empty()) fwrite(piece[h].text.data(), 1, piece[h].text.size(), fp);
      std::string().swap(piece[h].text);
    }
  });

  std::atomic<int> next{0};
  int ht = T < kH ? T : kH;
  run(ht, [&](int) {
    for (;;) {
      int h = next.fetch_add(1, std::memory_order_relaxed);
      if (h >= kH) break;
      uint32_t cnt[kCap] = {}, er[kCap] = {}, N = 0;
      for (int w = 0; w < T; ++w)
        for (B* b = ws[w].hour[h].head; b; b = b->next)
          for (uint32_t i = 0; i < b->n; ++i) {
            uint32_t g = rank[w * kCap + (b->r[i].meta & 0x7fff)];
            cnt[g]++;
            er[g] += b->r[i].meta >> 15;
            ++N;
          }
      std::string text;
      if (N) {
        uint32_t pref[kCap], cur[kCap], s = 0;
        for (uint32_t g = 0; g < nids; ++g) {
          pref[g] = cur[g] = s;
          s += cnt[g];
        }
        std::vector<uint32_t> lat(N);
        for (int w = 0; w < T; ++w)
          for (B* b = ws[w].hour[h].head; b; b = b->next)
            for (uint32_t i = 0; i < b->n; ++i)
              lat[cur[rank[w * kCap + (b->r[i].meta & 0x7fff)]]++] = b->r[i].lat;
        for (int w = 0; w < T; ++w) ws[w].hour[h].clear();
        const char* bp = blob.data();
        for (uint32_t g = 0; g < nids; ++g) {
          if (!cnt[g]) continue;
          uint32_t p50, p95, p99;
          select3(lat.data() + pref[g], cnt[g], p50, p95, p99);
          char line[192];
          char* o = line;
          memcpy(o, hours[h], 20);
          o += 20;
          *o++ = ' ';
          memcpy(o, bp + eoff[g], elen[g]);
          o += elen[g];
          o = putu(o, cnt[g]);
          o = putu(o, p50);
          o = putu(o, p95);
          o = putu(o, p99);
          o = putu(o, er[g]);
          o = putu(o, (uint32_t)((uint64_t)er[g] * 1000000ull / cnt[g]));
          *o++ = '\n';
          text.append(line, (size_t)(o - line));
        }
      } else {
        for (int w = 0; w < T; ++w) ws[w].hour[h].clear();
      }
      piece[h].text = std::move(text);
      piece[h].ready.store(1, std::memory_order_release);
    }
  });
  writer.join();
  fclose(fp);
  malloc_trim(0);
  return 0;
}
