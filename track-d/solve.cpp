// clang-format off
// Forward Dijkstra. SWAR parse, 512-node BFS blobs, Dial queue, paged distances.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <malloc.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
using u8 = uint8_t; using u32 = uint32_t; using u64 = uint64_t; constexpr auto REL = std::memory_order_relaxed;
constexpr int kBlob = 512; constexpr u32 kDial = 8192; constexpr u64 kInf = ~0ull;

[[noreturn]] void die(const char *m) {
  fprintf(stderr, "track-d: %s\n", m); exit(1);
}

int threads() {
  if (const char *e = std::getenv("SD_THREADS")) {
    int n = std::atoi(e); if (n >= 1 && n <= 64) return n;
  }
  unsigned h = std::thread::hardware_concurrency(); if (!h) return 1; return h > 12 ? 12 : (int)h;
}

template <class T> T *mapn(size_t n) {
  if (!n) n = 1; void *p = mmap(nullptr, n * sizeof(T), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) die("mmap"); return (T *)p;
}

void unmap(void *p, size_t n, size_t z) {
  if (p) munmap(p, (n ? n : 1) * z);
}

template <class F> void run(int T, F fn) {
  std::vector<std::thread> pool; pool.reserve(T);
  for (int t = 0; t < T; t++) pool.emplace_back(fn, t); for (auto &th : pool) th.join();
}

struct Graph {
  u32 n = 0, m = 0; u32 *ofs = nullptr, *hd = nullptr, *wt = nullptr, *inb = nullptr;
  bool has_in(u32 v) const { return (inb[v >> 5] >> (v & 31)) & 1u; }
};

struct Store {
  u64 **tab = nullptr; u64 *slab = nullptr; u64 **spare = nullptr; u32 *live = nullptr;
  int nspare = 0, nlive = 0, pool = 0;
  void init(u32 chunks) {
    pool = (int)chunks; tab = mapn<u64 *>(chunks); slab = mapn<u64>((size_t)chunks * kBlob); spare = mapn<u64 *>(chunks);
    live = mapn<u32>(chunks); for (int i = 0; i < pool; i++) spare[i] = slab + (size_t)i * kBlob; nspare = pool;
  }
  void reset() {
    for (int i = 0; i < nlive; i++) {
      spare[nspare++] = tab[live[i]]; tab[live[i]] = nullptr;
    }
    nlive = 0;
  }
  u64 &at(u32 v) {
    u32 c = v >> 9; u64 *&p = tab[c];
    if (!p) {
      if (!nspare) die("pages"); p = spare[--nspare]; memset(p, 0xFF, kBlob * sizeof(u64)); live[nlive++] = c;
    }
    return p[v & 511];
  }
};

struct Dial {
  std::vector<u32> b[kDial]; std::vector<int> touched; std::vector<u64> far_d; std::vector<u32> far_v; u64 cur = 0;
  void clear() {
    for (int i : touched) b[i].clear(); touched.clear(); far_d.clear(); far_v.clear(); cur = 0;
  }
  void push(u64 d, u32 v) {
    if (d - cur >= kDial) {
      far_d.push_back(d); far_v.push_back(v); return;
    }
    u32 i = (u32)d & (kDial - 1); if (b[i].empty()) touched.push_back((int)i); b[i].push_back(v);
  }
  bool take_far(u64 &d, u32 &v) {
    if (far_d.empty()) return false; size_t best = 0;
    for (size_t i = 1; i < far_d.size(); i++) if (far_d[i] < far_d[best]) best = i; d = far_d[best]; v = far_v[best];
    far_d[best] = far_d.back(); far_v[best] = far_v.back(); far_d.pop_back(); far_v.pop_back(); return true;
  }
};

int64_t dijkstra(const Graph &g, Store &st, Dial &h, u32 s, u32 t) {
  if (s == t) return 0; if (g.ofs[s] == g.ofs[s + 1]) return -1; if (!g.has_in(t)) return -1; st.reset();
  h.clear(); st.at(s) = 0; h.push(0, s); const u32 *ofs = g.ofs; const u32 *hd = g.hd; const u32 *wt = g.wt;
  while (true) {
    u32 bi = (u32)h.cur & (kDial - 1);
    if (h.b[bi].empty()) {
      u32 step = 1; while (step < kDial && h.b[(h.cur + step) & (kDial - 1)].empty()) ++step;
      if (step < kDial) {
        h.cur += step; continue;
      }
      u64 fd; u32 fv; if (!h.take_far(fd, fv)) return -1; if (st.at(fv) != fd) continue;
      h.cur = fd; if (fv == t) return (int64_t)h.cur; u32 u = fv;
      for (u32 i = ofs[u]; i < ofs[u + 1]; i++) {
        u32 v = hd[i]; u64 nd = h.cur + wt[i]; u64 &dv = st.at(v);
        if (nd < dv) {
          dv = nd; h.push(nd, v);
        }
      }
      continue;
    }
    u32 u = h.b[bi].back(); h.b[bi].pop_back(); if (st.at(u) != h.cur) continue; if (u == t) return (int64_t)h.cur;
    u32 end = ofs[u + 1];
    for (u32 i = ofs[u]; i < end; i++) {
      u32 v = hd[i]; u64 nd = h.cur + wt[i]; u64 &dv = st.at(v);
      if (nd < dv) {
        dv = nd; h.push(nd, v);
      }
    }
  }
}

u32 dig8(u64 v) {
  v = (v & 0x0F0F0F0F0F0F0F0Full) * 2561 >> 8; v = (v & 0x00FF00FF00FF00FFull) * 6553601 >> 16;
  v = (v & 0x0000FFFF0000FFFFull) * 42949672960001ull >> 32; return (u32)v;
}

int lead(u64 v) {
  u64 hi = (v & 0xF0F0F0F0F0F0F0F0ull) ^ 0x3030303030303030ull;
  u64 lo = ((v & 0x0F0F0F0F0F0F0F0Full) + 0x0606060606060606ull) & 0xF0F0F0F0F0F0F0F0ull; u64 bad = hi | lo;
  if (!bad) return 8; return __builtin_ctzll(bad) >> 3;
}

int g_fd = -1; off_t g_edge = 0, g_end = 0;

struct Scan {
  off_t pos, end; char buf[1 << 16]; int n = 0, i = 0;
  Scan(off_t b, off_t e) : pos(b), end(e) {}
  void fill() {
    int left = n - i; if (left > 0 && i) memmove(buf, buf + i, left);
    else left = left > 0 ? left : 0; size_t room = (sizeof(buf) - 16) - (size_t)left;
    off_t want = end - pos; if ((off_t)room > want) room = (size_t)want;
    ssize_t r = room ? ::pread(g_fd, buf + left, room, pos) : 0; if (r < 0) die("pread"); pos += r; n = left + (int)r;
    memset(buf + n, 0, 8); i = 0;
  }
  bool num(u32 &o) {
    if (i + 16 >= n) fill(); if (i >= n || (unsigned char)buf[i] < '0') return false; u64 v; memcpy(&v, buf + i, 8);
    int k = lead(v);
    if (k >= 8) {
      o = dig8(v); i += 8; while (i < n && (unsigned char)buf[i] >= '0' && buf[i] <= '9') o = o * 10u + buf[i++] - '0';
    } else {
      o = 0;
      for (int t = 0; t < k; t++) {
        o = o * 10u + (u32)((v & 255) - '0'); v >>= 8;
      }
      i += k;
    }
    if (i < n && buf[i]) ++i; return true;
  }
};

template <class F> void parse_range(off_t b, off_t e, F fn) {
  if (b >= e) return; Scan sc(b, e);
  if (b != g_edge) {
    while (sc.i >= sc.n) {
      sc.fill(); if (sc.i >= sc.n) return;
    }
    while (sc.i < sc.n && sc.buf[sc.i] != '\n') ++sc.i; if (sc.i < sc.n) ++sc.i;
  }
  u32 u, v, w; while (sc.num(u) && sc.num(v) && sc.num(w)) if (u != v) fn(u, v, w);
}

template <class F> void parse_all(int T, const std::vector<off_t> &cut, F fn) {
  run(T, [&](int t) { parse_range(cut[(size_t)t], cut[(size_t)t + 1], fn); });
}

void blobs(u32 n, u32 *ofs, u32 *hd, u32 *id, int T) {
  auto *claimed = mapn<std::atomic<u8>>(n); memset(claimed, 0, n); std::vector<std::vector<u32>> chain((size_t)T);
  run(T, [&](int t) {
    auto &order = chain[(size_t)t]; order.reserve((n / (u32)T) + kBlob);
    std::vector<u32> seeds; u32 lo = (u32)((u64)n * t / T); u32 hi = (u32)((u64)n * (t + 1) / T); u32 scan = lo;
    auto claim = [&](u32 v) {
      u8 no = 0; return claimed[v].compare_exchange_strong(no, 1, REL);
    };
    for (;;) {
      u32 seed = n;
      while (!seeds.empty() && seed == n) {
        u32 s = seeds.back(); seeds.pop_back(); if (claim(s)) seed = s;
      }
      while (seed == n && scan < hi) {
        u32 s = scan++; if (ofs[s] != ofs[s + 1] && claim(s)) seed = s;
      }
      if (seed == n) break; size_t base = order.size(); size_t i = base; order.push_back(seed);
      for (; i < order.size() && order.size() - base < (size_t)kBlob; ++i) {
        u32 u = order[i]; for (u32 e = ofs[u];
             e < ofs[u + 1] && order.size() - base < (size_t)kBlob; ++e)
          if (claim(hd[e])) order.push_back(hd[e]);
      }
      for (; i < order.size(); ++i) {
        u32 u = order[i]; for (u32 e = ofs[u]; e < ofs[u + 1]; ++e) if (!claimed[hd[e]].load(REL)) seeds.push_back(hd[e]);
      }
    }
  }); u32 k = 0;
  for (auto &order : chain) {
    for (u32 u : order) id[u] = k++;
  }
  for (u32 u = 0; u < n; u++) if (!claimed[u].load(REL)) id[u] = k++; unmap(claimed, n, 1); if (k != n) die("order");
}

void load(const char *path, Graph &g, u32 *&id) {
  g_fd = ::open(path, O_RDONLY); if (g_fd < 0) die("open graph"); struct stat st; if (fstat(g_fd, &st)) die("fstat");
  char hdr[64]{};
  if (::pread(g_fd, hdr, 63, 0) < 2) die("header"); char *hp = hdr;
  auto hnum = [&] {
    u32 x = 0; while (*hp >= '0') x = x * 10u + (u32)(*hp++ - '0'); if (*hp) ++hp; return x;
  }; g.n = hnum(); hnum(); g_edge = 0; while (hdr[g_edge] && hdr[g_edge] != '\n') ++g_edge; ++g_edge; g_end = st.st_size;

  int T = threads(); if (g_end - g_edge < (1 << 20)) T = 1; std::vector<off_t> cut((size_t)T + 1); cut[0] = g_edge;
  cut[(size_t)T] = g_end; off_t span = g_end - g_edge;
  for (int t = 1; t < T; t++) {
    off_t x = g_edge + span * t / T; char b[96]; ssize_t r = ::pread(g_fd, b, 95, x); int k = 0;
    while (k < r && b[k] != '\n') ++k; cut[(size_t)t] = x + (k < r ? k + 1 : 0);
    if (cut[(size_t)t] < cut[(size_t)t - 1]) cut[(size_t)t] = cut[(size_t)t - 1];
  }

  u32 n = g.n; g.ofs = mapn<u32>(n + 1); memset(g.ofs, 0, (n + 1ull) * 4); auto *deg = (std::atomic<u32> *)g.ofs;
  parse_all(T, cut, [&](u32 u, u32, u32) { deg[u + 1].fetch_add(1, REL); });
  for (u32 i = 1; i <= n; i++) g.ofs[i] += g.ofs[i - 1]; g.m = g.ofs[n];
  g.hd = mapn<u32>(g.m ? g.m : 1); g.wt = mapn<u32>(g.m ? g.m : 1);
  auto *cur = (std::atomic<u32> *)mapn<u32>(n); for (u32 i = 0; i < n; i++) cur[i].store(g.ofs[i], REL);
  parse_all(T, cut, [&](u32 u, u32 v, u32 w) {
    u32 p = cur[u].fetch_add(1, REL); g.hd[p] = v; g.wt[p] = w;
  }); unmap(cur, n, 4); close(g_fd);

  id = mapn<u32>(n); blobs(n, g.ofs, g.hd, id, threads());

  u32 *nofs = mapn<u32>(n + 1); memset(nofs, 0, (n + 1ull) * 4); int Tn = threads();
  run(Tn, [&](int t) {
    u32 lo = (u32)((u64)n * t / Tn); u32 hi = (u32)((u64)n * (t + 1) / Tn);
    for (u32 u = lo; u < hi; u++) nofs[id[u] + 1] = g.ofs[u + 1] - g.ofs[u];
  }); for (u32 i = 1; i <= n; i++) nofs[i] += nofs[i - 1];

  u32 *nwt = mapn<u32>(g.m ? g.m : 1);
  run(Tn, [&](int t) {
    u32 lo = (u32)((u64)n * t / Tn); u32 hi = (u32)((u64)n * (t + 1) / Tn);
    for (u32 u = lo; u < hi; u++) {
      u32 j = nofs[id[u]]; for (u32 e = g.ofs[u]; e < g.ofs[u + 1]; e++) nwt[j++] = g.wt[e];
    }
  }); unmap(g.wt, g.m ? g.m : 1, 4);

  u32 words = (n + 31) / 32 + 1; g.inb = mapn<u32>(words);
  memset(g.inb, 0, words * 4ull); auto *ib = (std::atomic<u32> *)g.inb; u32 *nhd = mapn<u32>(g.m ? g.m : 1);
  run(Tn, [&](int t) {
    u32 lo = (u32)((u64)n * t / Tn); u32 hi = (u32)((u64)n * (t + 1) / Tn);
    for (u32 u = lo; u < hi; u++) {
      u32 j = nofs[id[u]];
      for (u32 e = g.ofs[u]; e < g.ofs[u + 1]; e++) {
        u32 v = id[g.hd[e]]; nhd[j++] = v; ib[v >> 5].fetch_or(1u << (v & 31), REL);
      }
    }
  }); unmap(g.hd, g.m ? g.m : 1, 4); unmap(g.ofs, n + 1, 4); g.ofs = nofs; g.hd = nhd; g.wt = nwt;
}

void append_i64(char *&o, int64_t x) {
  if (x < 0) {
    *o++ = '-'; x = -x;
  }
  char tmp[24]; int n = 0; u64 u = (u64)x; do tmp[n++] = (char)('0' + u % 10);
  while (u /= 10); while (n--) *o++ = tmp[n]; *o++ = '\n';
}

const char *env_or(const char *k, const char *d) {
  const char *v = std::getenv(k); return v && *v ? v : d;
}

} // namespace

int main() {
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0); mallopt(M_MMAP_THRESHOLD, 64 * 1024);
  mallopt(M_TRIM_THRESHOLD, 64 * 1024); std::string dir = env_or("DATA", "/data");
  std::string outp = std::string(env_or("OUT", "/out")) + "/distances.txt"; Graph g;
  u32 *id = nullptr; load((dir + "/graph.txt").c_str(), g, id);

  int qfd = ::open((dir + "/queries.txt").c_str(), O_RDONLY); if (qfd < 0) die("open queries");
  struct stat qs; if (fstat(qfd, &qs)) die("fstat q"); char *qb = mapn<char>((size_t)qs.st_size + 1); size_t got = 0;
  while (got < (size_t)qs.st_size) {
    ssize_t r = ::read(qfd, qb + got, (size_t)qs.st_size - got); if (r <= 0) die("read q"); got += (size_t)r;
  }
  close(qfd); qb[got] = 0; std::vector<u32> sxs, txs;
  for (char *p = qb; *p;) {
    u32 s = 0, t = 0; while (*p >= '0') s = s * 10u + (u32)(*p++ - '0');
    if (*p) ++p; while (*p >= '0') t = t * 10u + (u32)(*p++ - '0'); if (*p) ++p; sxs.push_back(id[s]); txs.push_back(id[t]);
  }
  unmap(qb, (size_t)qs.st_size + 1, 1); unmap(id, g.n, 4);

  int Q = (int)sxs.size(); std::vector<int> ord(Q); for (int i = 0; i < Q; i++) ord[i] = i;
  std::sort(ord.begin(), ord.end(), [&](int a, int b) { return sxs[a] < sxs[b]; });

  int T = threads(); if (T > Q) T = Q; if (T < 1) T = 1; std::vector<int64_t> ans(Q);
  std::atomic<int> next{0};
  u32 chunks = (g.n + kBlob - 1) / kBlob;
  run(T, [&](int) {
    Store st; st.init(chunks); Dial h;
    for (;;) {
      int k = next.fetch_add(1, REL); if (k >= Q) break; int i = ord[k]; ans[i] = dijkstra(g, st, h, sxs[i], txs[i]);
    }
  });

  char *out = mapn<char>((size_t)Q * 24 + 8); char *op = out;
  for (int64_t d : ans) append_i64(op, d); int ofd = ::open(outp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (ofd < 0) die("open out"); size_t left = (size_t)(op - out); const char *wp = out;
  while (left) {
    ssize_t k = ::write(ofd, wp, left); if (k < 0) die("write"); wp += k; left -= (size_t)k;
  }
  close(ofd); return 0;
}
// clang-format on
