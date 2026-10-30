// Track A: tail scan, per-hour 6-byte records, in-cache select, writer thread.
// Nearest rank is (p * n + 99) / 100 - 1. A 5xx is status[0] == '5'.
// clang-format off

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
#include <vector>

using u32 = uint32_t;
using u64 = uint64_t;

constexpr int kH = 72;
constexpr u32 kCap = 2048, kSlots = 4096;

[[noreturn]] void die(const char* m) {
  fprintf(stderr, "track-a: %s\n", m), exit(1);
}

void tune() {
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
  mallopt(M_TRIM_THRESHOLD, 64 * 1024);
  mallopt(M_MMAP_THRESHOLD, 128 * 1024);
  mallopt(M_TOP_PAD, 0);
}

int nthreads() {
  const char* e = getenv("SD_THREADS");
  int n = e ? atoi(e) : 0;
  unsigned h = std::thread::hardware_concurrency();
  return n >= 1 && n <= 64 ? n : !h ? 1 : h > 12 ? 12 : (int)h;
}

void drop_pages(const char* base, size_t a, size_t b) {
  uintptr_t x = ((uintptr_t)(base + a) + 4095) & ~(uintptr_t)4095;
  uintptr_t y = (uintptr_t)(base + b) & ~(uintptr_t)4095;
  y > x && madvise((void*)x, y - x, MADV_DONTNEED);
}

struct __attribute__((packed)) Rec { u32 lat; uint16_t meta; };
struct B { B* next; u32 n; Rec r[1024]; };

struct Hour {
  B* head = nullptr;
  B* tail = nullptr;
  [[gnu::always_inline]] void add(u32 lat, uint16_t meta) {
    if (!tail || tail->n == 1024) {
      auto* b = (B*)malloc(sizeof(B));
      !b && (die("oom"), 0);
      b->next = nullptr, b->n = 0, (tail ? tail->next : head) = b, tail = b;
    }
    tail->r[tail->n++] = Rec{lat, meta};
  }
  void clear() {
    for (B *p = head, *n; p; p = n) n = p->next, free(p);
    head = tail = nullptr;
  }
};

struct Slot { u64 h = 0; u32 off = 0, len = 0, id = 0; };

struct Worker {
  std::string bytes;
  Slot slots[kSlots]{};
  u32 nids = 0;
  const char* blob = nullptr;
  Hour hour[kH];
  [[gnu::always_inline]] u32 intern(const char* s, u32 n, u64 h) {
    for (u32 i = (u32)h & (kSlots - 1);; i = (i + 1) & (kSlots - 1)) {
      Slot& sl = slots[i];
      if (sl.h == h && sl.len == n && !memcmp(blob + sl.off, s, n)) return sl.id;
      if (sl.h) continue;
      nids >= kCap && (die("endpoints"), 0);
      sl = Slot{h, (u32)bytes.size(), n, nids++};
      return bytes.append(s, n), blob = bytes.data(), sl.id;
    }
  }
};

[[gnu::always_inline]] inline int hour_of(const char* t) {
  int mo = t[6] - '0', day = (t[8] - '0') * 10 + (t[9] - '0');
  int hr = (t[11] - '0') * 10 + (t[12] - '0');
  return (mo ? day + 1 : day - 30) * 24 + hr - 18;
}

[[gnu::always_inline]] inline int space_at(u64 v) {
  u64 x = v ^ 0x2020202020202020ull;
  u64 y = (x - 0x0101010101010101ull) & ~x & 0x8080808080808080ull;
  return __builtin_ctzll(y) >> 3;
}

[[gnu::always_inline]] inline u64 hash_ep(const char* s, u32 n) {
  u64 a = 0, b = 0, m = 0;
  memcpy(&a, s, 8);
  n < 8 && (a &= (1ull << (n * 8)) - 1ull, 0);
  n > 8 && (memcpy(&b, s + n - 8, 8), memcpy(&m, s + (n / 2) - 4, 8), 0);
  u64 h = _mm_crc32_u64(_mm_crc32_u64(_mm_crc32_u64(n, a), m), b);
  return h ? h : 1;
}

struct Div { u64 m; unsigned s; };
static constexpr Div kDiv[9] = {
    {90071993ull, 53}, {112589991ull, 50}, {140737489ull, 47}, {87960931ull, 43},
    {109951163ull, 40}, {68719477ull, 36}, {42949673ull, 32}, {429496730ull, 32},
    {4294967296ull, 32}};

[[gnu::always_inline]] inline u32 parse_lat(const char* d, int k, const char* lim) {
  if (k <= 8 && d + 8 <= lim) {
    u64 v;
    memcpy(&v, d, 8);
    v &= (k == 8 ? ~0ull : (1ull << (k * 8)) - 1ull) & 0x0F0F0F0F0F0F0F0Full;
    u32 x = 0;
    for (int i = 0; i < 8; ++i) x = x * 10u + (u32)((v >> (8 * i)) & 255u);
    return (u32)((x * kDiv[k].m) >> kDiv[k].s);
  }
  u32 x = 0;
  for (int i = 0; i < k; ++i) x = x * 10u + (u32)(d[i] - '0');
  return x;
}

[[gnu::always_inline]] inline void take(Worker& w, const char* line, const char* nl, const char* lim) {
  const char* win = nl - 16;
  __m128i v = _mm_loadu_si128((const __m128i*)win);
  u32 sm = (u32)_mm_movemask_epi8(_mm_cmpeq_epi8(v, _mm_set1_epi8(' ')));
  const char* sp = win + (31 - __builtin_clz(sm));
  u64 mw;
  memcpy(&mw, line + 28, 8);
  const char* ep = line + 29 + space_at(mw);
  u32 elen = (u32)(sp - 4 - ep), id;
  int kd = (int)(nl - sp - 1);
  id = w.intern(ep, elen, hash_ep(ep, elen));
  w.hour[hour_of(line)].add(parse_lat(sp + 1, kd, lim),
                            (uint16_t)(id | ((sp[-3] == '5') << 15)));
}

void parse_slice(Worker& w, const char* base, size_t begin, size_t end, size_t flen) {
  const char* lim = base + flen;
  const char *line = base + begin, *e = base + end, *p = line;
  const __m256i nlv = _mm256_set1_epi8('\n');
  size_t dropped = begin;
  while (p + 32 <= e) {
    u32 m = (u32)_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i*)p), nlv));
    while (m) {
      const char* nl = p + __builtin_ctz(m);
      take(w, line, nl, lim), line = nl + 1, m &= m - 1;
    }
    p += 32;
    size_t at = (size_t)(line - base);
    at > dropped + (1u << 20) && (drop_pages(base, dropped, at - 4096), dropped = at - 4096, 0);
  }
  while (line < e) {
    const char* nl = line;
    for (; nl < e && *nl != '\n'; ++nl);
    nl < e ? (take(w, line, nl, lim), line = nl + 1) : (line = e);
  }
  drop_pages(base, dropped, end);
}

void isort(u32* a, u32 n) {
  for (u32 i = 1; i < n; ++i) {
    u32 v = a[i], j = i;
    while (j && a[j - 1] > v) a[j] = a[j - 1], --j;
    a[j] = v;
  }
}

void select3(u32* a, u32 n, u32& a50, u32& a95, u32& a99) {
  u32 i50 = (u32)((50ull * n + 99) / 100 - 1);
  u32 i95 = (u32)((95ull * n + 99) / 100 - 1);
  u32 i99 = (u32)((99ull * n + 99) / 100 - 1);
  n <= 48 ? isort(a, n) : (std::nth_element(a, a + i99, a + n),
                          std::nth_element(a, a + i95, a + i99 + 1),
                          std::nth_element(a, a + i50, a + i95 + 1));
  a50 = a[i50], a95 = a[i95], a99 = a[i99];
}

[[gnu::always_inline]] inline char* putu(char* o, u32 x) {
  *o++ = ' ';
  char t[10];
  int n = 0;
  do t[n++] = (char)('0' + x % 10);
  while (x /= 10);
  while (n--) *o++ = t[n];
  return o;
}

struct Piece { std::string text; std::atomic<int> ready{0}; };

template <class F>
void run(int n, F fn) {
  std::vector<std::thread> pool;
  for (int t = 0; t < n; ++t) pool.emplace_back(fn, t);
  for (auto& th : pool) th.join();
}

int main() {
  tune();
  const char *data = getenv("DATA"), *outd = getenv("OUT");
  std::string in = std::string(data && *data ? data : "/data") + "/requests.log";
  std::string outp = std::string(outd && *outd ? outd : "/out") + "/summary.txt";
  int fd = open(in.c_str(), O_RDONLY);
  struct stat st {};
  fd < 0 && (die("open"), 0);
  fstat(fd, &st) && (die("stat"), 0);
  size_t flen = (size_t)st.st_size;
  FILE* fp = fopen(outp.c_str(), "wb");
  !fp && (die("out"), 0);
  if (!flen) return fclose(fp), 0;
  void* mapped = mmap(nullptr, flen, PROT_READ, MAP_PRIVATE, fd, 0);
  mapped == MAP_FAILED && (die("mmap"), 0);
  madvise(mapped, flen, MADV_RANDOM);
  const char* file = (const char*)mapped;
  int T = nthreads();
  std::vector<size_t> cut(T + 1);
  cut[0] = 0, cut[T] = flen;
  for (int t = 1; t < T; ++t) {
    size_t pos = flen * (size_t)t / (size_t)T;
    for (; pos && pos < flen && file[pos - 1] != '\n'; ++pos);
    cut[t] = pos;
  }
  std::vector<Worker> ws(T);
  for (int t = 0; t < T; ++t) ws[t].bytes.reserve(1 << 16);
  run(T, [&](int t) {
    if (cut[t] < cut[t + 1]) parse_slice(ws[t], file, cut[t], cut[t + 1], flen);
  });
  drop_pages(file, 0, flen);

  struct Name { const char* s; u32 n; uint16_t tid, lid; };
  std::vector<Name> names;
  for (int t = 0; t < T; ++t)
    for (const Slot& sl : ws[t].slots)
      sl.h && (names.push_back({ws[t].bytes.data() + sl.off, sl.len, (uint16_t)t,
                                (uint16_t)sl.id}),
               0);
  std::sort(names.begin(), names.end(), [](const Name& a, const Name& b) {
    int c = memcmp(a.s, b.s, a.n < b.n ? a.n : b.n);
    return c ? c < 0 : a.n < b.n;
  });
  std::string blob;
  blob.reserve(1 << 16);
  std::vector<u32> eoff, elen, rank(T * kCap);
  u32 nids = 0;
  for (size_t i = 0; i < names.size(); ++i) {
    bool same = i && names[i].n == names[i - 1].n && !memcmp(names[i].s, names[i - 1].s, names[i].n);
    if (!same) {
      eoff.push_back((u32)blob.size()), elen.push_back(names[i].n);
      blob.append(names[i].s, names[i].n), ++nids;
    }
    rank[names[i].tid * kCap + names[i].lid] = nids - 1;
  }

  char hours[kH][21];
  for (int i = 0; i < kH; ++i) {
    int tot = i + 18, day = tot / 24, hr = tot % 24, month = day < 2 ? 10 : 11;
    int dom = day == 0 ? 30 : day == 1 ? 31 : day - 1;
    snprintf(hours[i], 21, "%04d-%02d-%02dT%02d:00:00Z", 2026, month, dom, hr);
  }

  Piece piece[kH];
  std::thread writer([&] {
    for (int h = 0; h < kH; ++h) {
      while (!piece[h].ready.load(std::memory_order_acquire)) _mm_pause();
      !piece[h].text.empty() &&
          fwrite(piece[h].text.data(), 1, piece[h].text.size(), fp);
      std::string().swap(piece[h].text);
    }
  });

  std::atomic<int> next{0};
  int ht = T < kH ? T : kH;
  run(ht, [&](int) {
    for (int h; (h = next.fetch_add(1, std::memory_order_relaxed)) < kH;) {
      u32 cnt[kCap] = {}, er[kCap] = {}, N = 0, g;
      for (int w = 0; w < T; ++w)
        for (B* b = ws[w].hour[h].head; b; b = b->next)
          for (u32 i = 0; i < b->n; ++i)
            g = rank[w * kCap + (b->r[i].meta & 0x7fff)], cnt[g]++,
            er[g] += b->r[i].meta >> 15, ++N;
      std::string text;
      if (N) {
        u32 pref[kCap], cur[kCap], s = 0;
        for (u32 g = 0; g < nids; ++g) pref[g] = cur[g] = s, s += cnt[g];
        std::vector<u32> lat(N);
        for (int w = 0; w < T; ++w)
          for (B* b = ws[w].hour[h].head; b; b = b->next)
            for (u32 i = 0; i < b->n; ++i)
              lat[cur[rank[w * kCap + (b->r[i].meta & 0x7fff)]]++] = b->r[i].lat;
        const char* bp = blob.data();
        for (u32 g = 0; g < nids; ++g) if (cnt[g]) {
          u32 p50, p95, p99;
          select3(lat.data() + pref[g], cnt[g], p50, p95, p99);
          char line[192], *o = line;
          memcpy(o, hours[h], 20), o += 20, *o++ = ' ';
          memcpy(o, bp + eoff[g], elen[g]), o += elen[g];
          u32 ppm = (u32)((u64)er[g] * 1000000ull / cnt[g]);
          o = putu(putu(putu(putu(putu(putu(o, cnt[g]), p50), p95), p99), er[g]), ppm);
          *o++ = '\n', text.append(line, (size_t)(o - line));
        }
      }
      for (int w = 0; w < T; ++w) ws[w].hour[h].clear();
      piece[h].text = std::move(text), piece[h].ready.store(1, std::memory_order_release);
    }
  });
  writer.join(), fclose(fp), malloc_trim(0);
  return 0;
}
// clang-format on
