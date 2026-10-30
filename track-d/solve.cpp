// Track D: bidirectional Dijkstra on a packed CSR.
//
// The road graph is barely denser than a tree, so a bidirectional search
// meets after roughly the square root of a one-sided ball. Both directions
// use a 4-ary heap. Edges near 2^32 stay in a side list; a query they can
// win falls back to a 64-bit search. Parsing is parallel over the mapped text.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <thread>
#include <time.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr uint32_t kHuge = 0xFFF00000u;
constexpr uint32_t kSettled = 0x80000000u;
constexpr int64_t kNeed = INT64_MIN;

[[noreturn]] void die(const char* msg) {
  fprintf(stderr, "track-d: %s\n", msg);
  exit(1);
}

int env_int(const char* key, int def, int lo, int hi) {
  const char* e = std::getenv(key);
  if (!e || !*e) return def;
  int n = std::atoi(e);
  if (n < lo || n > hi) return def;
  return n;
}

int worker_count() {
  int n = env_int("SD_THREADS", 0, 1, 64);
  if (n) return n;
  unsigned hc = std::thread::hardware_concurrency();
  if (hc == 0) return 1;
  if (hc >= 12) return 6;
  return static_cast<int>(hc);
}

double now_ms() {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1e3 + ts.tv_nsec * 1e-6;
}

template <class T>
T* map_arr(size_t n) {
  if (n == 0) n = 1;
  void* p = mmap(nullptr, n * sizeof(T), PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED) die("mmap");
  return static_cast<T*>(p);
}

void unmap(void* p, size_t n, size_t elem) {
  if (!p) return;
  if (n == 0) n = 1;
  munmap(p, n * elem);
}

struct Edge {
  uint32_t u, v, w;
};

struct Huge {
  uint32_t u, v, w;
};

struct CSR {
  uint32_t* ofs = nullptr;
  uint64_t* e = nullptr;
  uint32_t m = 0;
};

struct Graph {
  uint32_t n = 0;
  CSR fwd, rev;
  uint32_t* inb = nullptr;
  Huge* huge = nullptr;
  int nhuge = 0;
  uint32_t max_small = 0;
  uint32_t min_huge = 0xffffffffu;
  uint32_t huge_from[96]{};
  int nhuge_from = 0;
  uint32_t* o2n = nullptr;

  bool has_in(uint32_t v) const { return (inb[v >> 5] >> (v & 31)) & 1u; }
  bool huge_out(uint32_t u) const {
    for (int i = 0; i < nhuge_from; i++)
      if (huge_from[i] == u) return true;
    return false;
  }
  void release() {
    unmap(fwd.ofs, static_cast<size_t>(n) + 1, sizeof(uint32_t));
    unmap(fwd.e, fwd.m ? fwd.m : 1, sizeof(uint64_t));
    unmap(rev.ofs, static_cast<size_t>(n) + 1, sizeof(uint32_t));
    unmap(rev.e, rev.m ? rev.m : 1, sizeof(uint64_t));
    unmap(inb, (static_cast<size_t>(n) + 31) / 32 + 1, sizeof(uint32_t));
    unmap(huge, nhuge ? static_cast<size_t>(nhuge) : 1, sizeof(Huge));
    unmap(o2n, n, sizeof(uint32_t));
  }
};

void bfs_order(const CSR& csr, uint32_t n, uint32_t* o2n) {
  uint8_t* seen = map_arr<uint8_t>(n);
  memset(seen, 0, n);
  uint32_t* q = map_arr<uint32_t>(n);
  uint32_t nid = 0;
  for (uint32_t s = 0; s < n; s++) {
    if (seen[s] || csr.ofs[s] == csr.ofs[s + 1]) continue;
    uint32_t qh = 0, qt = 0;
    seen[s] = 1;
    o2n[s] = nid++;
    q[qt++] = s;
    while (qh < qt) {
      uint32_t u = q[qh++];
      uint32_t a = csr.ofs[u], b = csr.ofs[u + 1];
      for (uint32_t i = a; i < b; i++) {
        uint32_t v = static_cast<uint32_t>(csr.e[i]);
        if (seen[v]) continue;
        seen[v] = 1;
        o2n[v] = nid++;
        q[qt++] = v;
      }
    }
  }
  for (uint32_t u = 0; u < n; u++)
    if (!seen[u]) o2n[u] = nid++;
  unmap(seen, n, 1);
  unmap(q, n, sizeof(uint32_t));
}

uint32_t take(const char*& p) {
  uint32_t x = static_cast<unsigned char>(*p++) - '0';
  while (static_cast<unsigned char>(*p) >= '0') x = x * 10u + static_cast<unsigned char>(*p++) - '0';
  ++p;
  return x;
}

Edge* parse_graph(const char* path, uint32_t& n, uint32_t& m, int threads) {
  int fd = ::open(path, O_RDONLY);
  if (fd < 0) die("open graph");
  struct stat st;
  if (fstat(fd, &st) != 0) die("fstat");
  size_t len = static_cast<size_t>(st.st_size);
  char* buf = map_arr<char>(len + 1);
  size_t got = 0;
  while (got < len) {
    ssize_t r = ::read(fd, buf + got, len - got);
    if (r <= 0) die("read graph");
    got += static_cast<size_t>(r);
  }
  close(fd);
  buf[len] = 0;
  const char* p = buf;
  n = take(p);
  m = take(p);
  const char* edge_beg = p;
  const char* edge_end = buf + len;
  if (threads < 1) threads = 1;
  if (threads > static_cast<int>(m > 0 ? m : 1)) threads = static_cast<int>(m > 0 ? m : 1);

  std::vector<const char*> starts(static_cast<size_t>(threads) + 1);
  starts[0] = edge_beg;
  size_t span = static_cast<size_t>(edge_end - edge_beg);
  for (int t = 1; t < threads; t++) {
    const char* s = edge_beg + span * static_cast<size_t>(t) / static_cast<size_t>(threads);
    while (s < edge_end && *s != '\n') ++s;
    if (s < edge_end) ++s;
    starts[static_cast<size_t>(t)] = s;
  }
  starts[static_cast<size_t>(threads)] = edge_end;

  std::vector<uint32_t> cnt(static_cast<size_t>(threads)), off(static_cast<size_t>(threads) + 1);
  std::vector<std::thread> pool;
  for (int t = 0; t < threads; t++) {
    pool.emplace_back([&, t] {
      const char* q = starts[static_cast<size_t>(t)];
      const char* e = starts[static_cast<size_t>(t) + 1];
      uint32_t c = 0;
      while (q < e) {
        const void* f = memchr(q, '\n', static_cast<size_t>(e - q));
        if (!f) break;
        ++c;
        q = static_cast<const char*>(f) + 1;
      }
      cnt[static_cast<size_t>(t)] = c;
    });
  }
  for (auto& th : pool) th.join();
  pool.clear();
  for (int t = 0; t < threads; t++) off[static_cast<size_t>(t) + 1] = off[static_cast<size_t>(t)] + cnt[static_cast<size_t>(t)];
  if (off.back() != m) die("edge count");

  Edge* edges = map_arr<Edge>(m ? m : 1);
  for (int t = 0; t < threads; t++) {
    pool.emplace_back([&, t] {
      const char* q = starts[static_cast<size_t>(t)];
      Edge* o = edges + off[static_cast<size_t>(t)];
      uint32_t c = cnt[static_cast<size_t>(t)];
      for (uint32_t i = 0; i < c; i++) {
        uint32_t u = take(q);
        uint32_t v = take(q);
        uint32_t w = take(q);
        o[i] = Edge{u, v, w};
      }
    });
  }
  for (auto& th : pool) th.join();
  unmap(buf, len + 1, 1);
  return edges;
}

enum class Key : int { U = 0, V = 1 };

Edge* radix_sort(Edge* a, Edge* tmp, uint32_t m, Key key) {
  Edge* src = a;
  Edge* dst = tmp;
  int T = m >= 200000 ? worker_count() : 1;
  if (T < 1) T = 1;
  // Two 16-bit passes. Node ids fit in 21 bits.
  std::vector<std::vector<uint32_t>> local(static_cast<size_t>(T), std::vector<uint32_t>(65536));
  for (int pass = 0; pass < 2; ++pass) {
    int shift = pass * 16;
    for (int t = 0; t < T; t++) std::fill(local[static_cast<size_t>(t)].begin(), local[static_cast<size_t>(t)].end(), 0);
    std::vector<std::thread> pool;
    auto field = [&](const Edge& e) { return (key == Key::U ? e.u : e.v) >> shift; };
    for (int t = 0; t < T; t++) {
      pool.emplace_back([&, t] {
        uint32_t b = static_cast<uint32_t>(static_cast<uint64_t>(m) * t / T);
        uint32_t e = static_cast<uint32_t>(static_cast<uint64_t>(m) * (t + 1) / T);
        uint32_t* c = local[static_cast<size_t>(t)].data();
        for (uint32_t i = b; i < e; i++) c[field(src[i]) & 65535]++;
      });
    }
    for (auto& th : pool) th.join();
    pool.clear();
    uint32_t base[65536];
    uint32_t sum = 0;
    for (int i = 0; i < 65536; i++) {
      base[i] = sum;
      for (int t = 0; t < T; t++) sum += local[static_cast<size_t>(t)][static_cast<size_t>(i)];
    }
    // Exclusive offset of each thread inside each bucket.
    for (int i = 0; i < 65536; i++) {
      uint32_t run = base[i];
      for (int t = 0; t < T; t++) {
        uint32_t c = local[static_cast<size_t>(t)][static_cast<size_t>(i)];
        local[static_cast<size_t>(t)][static_cast<size_t>(i)] = run;
        run += c;
      }
    }
    for (int t = 0; t < T; t++) {
      pool.emplace_back([&, t] {
        uint32_t b = static_cast<uint32_t>(static_cast<uint64_t>(m) * t / T);
        uint32_t e = static_cast<uint32_t>(static_cast<uint64_t>(m) * (t + 1) / T);
        uint32_t* c = local[static_cast<size_t>(t)].data();
        for (uint32_t i = b; i < e; i++) dst[c[field(src[i]) & 65535]++] = src[i];
      });
    }
    for (auto& th : pool) th.join();
    std::swap(src, dst);
  }
  return src;
}

uint32_t dedup_sorted(uint64_t* a, uint32_t n) {
  if (n <= 1) return n;
  if (n <= 48) {
    for (uint32_t i = 1; i < n; i++) {
      uint64_t x = a[i];
      uint32_t xv = static_cast<uint32_t>(x);
      uint32_t xw = static_cast<uint32_t>(x >> 32);
      uint32_t j = i;
      while (j > 0) {
        uint64_t y = a[j - 1];
        uint32_t yv = static_cast<uint32_t>(y);
        uint32_t yw = static_cast<uint32_t>(y >> 32);
        if (yv < xv || (yv == xv && yw <= xw)) break;
        a[j] = y;
        --j;
      }
      a[j] = x;
    }
  } else {
    std::sort(a, a + n, [](uint64_t x, uint64_t y) {
      uint32_t xv = static_cast<uint32_t>(x), yv = static_cast<uint32_t>(y);
      if (xv != yv) return xv < yv;
      return (x >> 32) < (y >> 32);
    });
  }
  uint32_t w = 0;
  for (uint32_t i = 0; i < n; i++) {
    uint32_t v = static_cast<uint32_t>(a[i]);
    uint32_t wt = static_cast<uint32_t>(a[i] >> 32);
    if (w && static_cast<uint32_t>(a[w - 1]) == v) {
      if (static_cast<uint32_t>(a[w - 1] >> 32) > wt) a[w - 1] = a[i];
    } else {
      a[w++] = a[i];
    }
  }
  return w;
}

void emit_csr(Edge* sorted, uint32_t m, uint32_t n, bool reverse, CSR& out, Graph& g) {
  out.ofs = map_arr<uint32_t>(static_cast<size_t>(n) + 1);
  out.e = map_arr<uint64_t>(m ? m : 1);
  uint32_t wri = 0, i = 0;
  for (uint32_t s = 0; s < n; s++) {
    out.ofs[s] = wri;
    while (i < m) {
      uint32_t key = reverse ? sorted[i].v : sorted[i].u;
      if (key >= s) break;
      ++i;
    }
    uint32_t start = wri;
    while (i < m) {
      uint32_t key = reverse ? sorted[i].v : sorted[i].u;
      if (key != s) break;
      uint32_t dst = reverse ? sorted[i].u : sorted[i].v;
      uint32_t w = sorted[i].w;
      ++i;
      if (dst == s) continue;
      if (!reverse) g.inb[dst >> 5] |= 1u << (dst & 31);
      if (w >= kHuge) continue;
      out.e[wri++] = static_cast<uint64_t>(dst) | (static_cast<uint64_t>(w) << 32);
      if (!reverse && w > g.max_small) g.max_small = w;
    }
    wri = start + dedup_sorted(out.e + start, wri - start);
  }
  out.ofs[n] = wri;
  out.m = wri;
  (void)g;
}

// Huge edges are collected in a separate pass so emit stays a straight scan.
void collect_huge(Edge* edges, uint32_t m, Graph& g) {
  int cap = 128;
  g.huge = map_arr<Huge>(static_cast<size_t>(cap));
  g.nhuge = 0;
  for (uint32_t i = 0; i < m; i++) {
    if (edges[i].u == edges[i].v) continue;
    if (edges[i].w < kHuge) continue;
    if (g.nhuge == cap) {
      Huge* nb = map_arr<Huge>(static_cast<size_t>(cap) * 2);
      memcpy(nb, g.huge, static_cast<size_t>(g.nhuge) * sizeof(Huge));
      unmap(g.huge, static_cast<size_t>(cap), sizeof(Huge));
      g.huge = nb;
      cap *= 2;
    }
    g.huge[g.nhuge++] = Huge{edges[i].u, edges[i].v, edges[i].w};
    if (edges[i].w < g.min_huge) g.min_huge = edges[i].w;
    g.inb[edges[i].v >> 5] |= 1u << (edges[i].v & 31);
    bool seen = false;
    for (int j = 0; j < g.nhuge_from; j++)
      if (g.huge_from[j] == edges[i].u) seen = true;
    if (!seen && g.nhuge_from < 96) g.huge_from[g.nhuge_from++] = edges[i].u;
  }
}

Graph build(Edge* edges, uint32_t m, uint32_t n) {
  Graph g;
  g.n = n;
  double b0 = now_ms();
  g.inb = map_arr<uint32_t>((static_cast<size_t>(n) + 31) / 32 + 1);
  memset(g.inb, 0, ((static_cast<size_t>(n) + 31) / 32 + 1) * sizeof(uint32_t));
  collect_huge(edges, m, g);
  double b1 = now_ms();
  Edge* tmp = map_arr<Edge>(m ? m : 1);
  Edge* by_u = radix_sort(edges, tmp, m, Key::U);
  double b2 = now_ms();
  emit_csr(by_u, m, n, false, g.fwd, g);
  double b3 = now_ms();
  int order = env_int("SD_ORDER", 1, 0, 1);
  if (order) {
    g.o2n = map_arr<uint32_t>(n);
    bfs_order(g.fwd, n, g.o2n);
    Edge* arr = by_u;
    for (uint32_t i = 0; i < m; i++) {
      arr[i].u = g.o2n[arr[i].u];
      arr[i].v = g.o2n[arr[i].v];
    }
    for (int h = 0; h < g.nhuge; h++) {
      g.huge[h].u = g.o2n[g.huge[h].u];
      g.huge[h].v = g.o2n[g.huge[h].v];
    }
    for (int h = 0; h < g.nhuge_from; h++) g.huge_from[h] = g.o2n[g.huge_from[h]];
    unmap(g.fwd.ofs, static_cast<size_t>(n) + 1, sizeof(uint32_t));
    unmap(g.fwd.e, m ? m : 1, sizeof(uint64_t));
    g.fwd = CSR{};
    memset(g.inb, 0, ((static_cast<size_t>(n) + 31) / 32 + 1) * sizeof(uint32_t));
    for (int h = 0; h < g.nhuge; h++) g.inb[g.huge[h].v >> 5] |= 1u << (g.huge[h].v & 31);
    Edge* dst0 = (by_u == edges) ? tmp : edges;
    by_u = radix_sort(arr, dst0, m, Key::U);
    emit_csr(by_u, m, n, false, g.fwd, g);
  }
  Edge* src = by_u;
  Edge* dst = (by_u == edges) ? tmp : edges;
  Edge* by_v = radix_sort(src, dst, m, Key::V);
  double b4 = now_ms();
  emit_csr(by_v, m, n, true, g.rev, g);
  double b5 = now_ms();
  unmap(edges, m ? m : 1, sizeof(Edge));
  unmap(tmp, m ? m : 1, sizeof(Edge));
  if (std::getenv("SD_PROF")) {
    fprintf(stderr, "  huge %.1f radixU %.1f emitF %.1f radixV %.1f emitR %.1f\n", b1 - b0, b2 - b1, b3 - b2,
            b4 - b3, b5 - b4);
    uint32_t hist[9] = {};
    for (uint32_t u = 0; u < n; u++) {
      uint32_t d = g.fwd.ofs[u + 1] - g.fwd.ofs[u];
      uint32_t di = g.rev.ofs[u + 1] - g.rev.ofs[u];
      hist[d < 8 ? d : 7]++;
      if (d == 1 && di == 1) hist[8]++;
    }
    fprintf(stderr, "  deg");
    for (int i = 0; i < 8; i++) fprintf(stderr, " %u:%u", i, hist[i]);
    fprintf(stderr, " chain %u\n", hist[8]);
  }
  return g;
}

struct Lab {
  uint32_t dist;
  uint32_t stamp;
};

struct Dial {
  int C = 64;
  uint32_t mask = 63;
  uint32_t** b = nullptr;
  int* len = nullptr;
  int* cap = nullptr;
  uint64_t* bits = nullptr;
  int nwords = 1;
  uint32_t cur = 0;
  int sz = 0;

  void init(int C_) {
    C = C_;
    mask = static_cast<uint32_t>(C - 1);
    nwords = C >> 6;
    b = map_arr<uint32_t*>(static_cast<size_t>(C));
    len = map_arr<int>(static_cast<size_t>(C));
    cap = map_arr<int>(static_cast<size_t>(C));
    bits = map_arr<uint64_t>(static_cast<size_t>(nwords));
    memset(b, 0, static_cast<size_t>(C) * sizeof(uint32_t*));
    memset(len, 0, static_cast<size_t>(C) * sizeof(int));
    memset(cap, 0, static_cast<size_t>(C) * sizeof(int));
    memset(bits, 0, static_cast<size_t>(nwords) * sizeof(uint64_t));
  }
  void clear() {
    if (sz) {
      memset(len, 0, static_cast<size_t>(C) * sizeof(int));
      memset(bits, 0, static_cast<size_t>(nwords) * sizeof(uint64_t));
    }
    cur = 0;
    sz = 0;
  }
  void grow(int i) {
    int nc = cap[i] ? cap[i] * 2 : 8;
    uint32_t* p = static_cast<uint32_t*>(realloc(b[i], static_cast<size_t>(nc) * sizeof(uint32_t)));
    if (!p) die("realloc");
    b[i] = p;
    cap[i] = nc;
  }
  void push(uint32_t d, uint32_t v) {
    int i = static_cast<int>(d & mask);
    if (len[i] == cap[i]) grow(i);
    b[i][len[i]++] = v;
    bits[i >> 6] |= 1ull << (i & 63);
    ++sz;
  }
  int next_bucket(int from) const {
    int w = from >> 6;
    int shift = from & 63;
    uint64_t x = bits[w] >> shift;
    if (x) return (w << 6) + shift + __builtin_ctzll(x);
    for (int k = w + 1; k < nwords; ++k)
      if (bits[k]) return (k << 6) + __builtin_ctzll(bits[k]);
    for (int k = 0; k <= w; ++k)
      if (bits[k]) return (k << 6) + __builtin_ctzll(bits[k]);
    return -1;
  }
  // Drop stale tails until the back of the current bucket is a live label at cur.
  bool peek(Lab* lab, uint32_t ep, uint32_t& d, uint32_t& u) {
    while (sz) {
      int i = static_cast<int>(cur & mask);
      if (len[i] == 0) {
        int j = next_bucket(i);
        if (j < 0) return false;
        int delta = (j - i) & static_cast<int>(mask);
        if (delta == 0) {
          bits[i >> 6] &= ~(1ull << (i & 63));
          continue;
        }
        cur += static_cast<uint32_t>(delta);
        continue;
      }
      u = b[i][len[i] - 1];
      Lab L = lab[u];
      if (L.stamp == ep && L.dist == cur) {
        d = cur;
        return true;
      }
      --len[i];
      --sz;
      if (len[i] == 0) bits[i >> 6] &= ~(1ull << (i & 63));
    }
    return false;
  }
  void pop_back() {
    int i = static_cast<int>(cur & mask);
    --len[i];
    --sz;
    if (len[i] == 0) bits[i >> 6] &= ~(1ull << (i & 63));
  }
};

int dial_modulus(uint32_t max_w) {
  unsigned x = max_w + 1;
  if (x <= 64) return 64;
  --x;
  x |= x >> 1;
  x |= x >> 2;
  x |= x >> 4;
  x |= x >> 8;
  x |= x >> 16;
  ++x;
  if (x > (1u << 16) || x <= max_w) return 0;
  return static_cast<int>(x);
}

struct Worker {
  const Graph* g = nullptr;
  Lab* lf = nullptr;
  Lab* lb = nullptr;
  uint32_t epoch = 1;
  Dial hf, hb;
  uint64_t expanded = 0;

  void init(const Graph* gp) {
    g = gp;
    lf = map_arr<Lab>(gp->n);
    lb = map_arr<Lab>(gp->n);
    memset(lf, 0, gp->n * sizeof(Lab));
    memset(lb, 0, gp->n * sizeof(Lab));
    int C = dial_modulus(gp->max_small);
    if (!C) die("weight too large for dial");
    hf.init(C);
    hb.init(C);
  }

  bool valid_top(Dial& h, Lab* lab, uint32_t& d, uint32_t& u) { return h.peek(lab, epoch, d, u); }

  void relax(bool forward, uint32_t u, uint32_t d, uint32_t& best) {
    const CSR& csr = forward ? g->fwd : g->rev;
    Lab* mine = forward ? lf : lb;
    Lab* other = forward ? lb : lf;
    uint32_t a = csr.ofs[u], b = csr.ofs[u + 1];
    const uint64_t* e = csr.e;
    for (uint32_t i = a; i < b; i++) {
      uint64_t ed = e[i];
      uint32_t v = static_cast<uint32_t>(ed);
      uint32_t w = static_cast<uint32_t>(ed >> 32);
      uint32_t nd = d + w;
      if (nd < d || nd >= best) continue;
      Lab& Lv = mine[v];
      if (Lv.stamp == (epoch | kSettled)) continue;
      if (Lv.stamp != epoch || nd < Lv.dist) {
        Lv.stamp = epoch;
        Lv.dist = nd;
        (forward ? hf : hb).push(nd, v);
        Lab Lo = other[v];
        if ((Lo.stamp & ~kSettled) == epoch) {
          uint32_t sum = nd + Lo.dist;
          if (sum >= nd && sum < best) best = sum;
        }
      }
    }
  }

  int64_t search(uint32_t s, uint32_t t) {
    const Graph& G = *g;
    if (s == t) return 0;
    if (G.fwd.ofs[s] == G.fwd.ofs[s + 1] && !G.huge_out(s)) return -1;
    if (!G.has_in(t)) return -1;
    if (++epoch == 0) {
      memset(lf, 0, G.n * sizeof(Lab));
      memset(lb, 0, G.n * sizeof(Lab));
      epoch = 1;
    }
    hf.clear();
    hb.clear();
    lf[s] = Lab{0, epoch};
    lb[t] = Lab{0, epoch};
    hf.push(0, s);
    hb.push(0, t);
    uint32_t best = 0xffffffffu;
    uint32_t nf = 0, nb = 0;
    while (hf.sz && hb.sz) {
      uint32_t df, db, uf, ub;
      if (!valid_top(hf, lf, df, uf) || !valid_top(hb, lb, db, ub)) break;
      uint64_t sum = static_cast<uint64_t>(df) + db;
      if (sum >= best) break;
      bool forward = nf <= nb;
      Dial& h = forward ? hf : hb;
      Lab* lab = forward ? lf : lb;
      Lab* other = forward ? lb : lf;
      uint32_t d = 0, u = 0;
      if (!h.peek(lab, epoch, d, u)) break;
      h.pop_back();
      if (lab[u].stamp != epoch || lab[u].dist != d) continue;
      lab[u].stamp = epoch | kSettled;
      ++expanded;
      if (forward) ++nf;
      else ++nb;
      Lab Lo = other[u];
      if ((Lo.stamp & ~kSettled) == epoch) {
        uint32_t path = d + Lo.dist;
        if (path >= d && path < best) best = path;
      }
      relax(forward, u, d, best);
    }
    if (best < G.min_huge) return static_cast<int64_t>(best);
    if (G.nhuge == 0) return -1;
    return kNeed;
  }
};

struct HeapW {
  uint64_t* key = nullptr;
  uint32_t* val = nullptr;
  int n = 0, cap = 0;
  void clear() { n = 0; }
  void push(uint64_t k, uint32_t v) {
    if (n == cap) {
      int nc = cap ? cap * 2 : 256;
      key = static_cast<uint64_t*>(realloc(key, static_cast<size_t>(nc) * sizeof(uint64_t)));
      val = static_cast<uint32_t*>(realloc(val, static_cast<size_t>(nc) * sizeof(uint32_t)));
      if (!key || !val) die("realloc");
      cap = nc;
    }
    int i = n++;
    while (i) {
      int p = (i - 1) >> 2;
      if (key[p] <= k) break;
      key[i] = key[p];
      val[i] = val[p];
      i = p;
    }
    key[i] = k;
    val[i] = v;
  }
  bool pop(uint64_t& k, uint32_t& v) {
    if (!n) return false;
    k = key[0];
    v = val[0];
    uint64_t dk = key[--n];
    uint32_t dv = val[n];
    if (!n) return true;
    int i = 0;
    for (;;) {
      int c = (i << 2) + 1;
      if (c >= n) break;
      int best = c;
      uint64_t bd = key[c];
      int last = c + 4;
      if (last > n) last = n;
      for (int j = c + 1; j < last; j++)
        if (key[j] < bd) {
          bd = key[j];
          best = j;
        }
      if (bd >= dk) break;
      key[i] = key[best];
      val[i] = val[best];
      i = best;
    }
    key[i] = dk;
    val[i] = dv;
    return true;
  }
};

int64_t search64w(const Graph& G, uint32_t s, uint32_t t, uint64_t* dist, uint32_t* stamp, uint32_t& epoch,
                  HeapW& heap) {
  if (s == t) return 0;
  if (G.fwd.ofs[s] == G.fwd.ofs[s + 1] && !G.huge_out(s)) return -1;
  if (!G.has_in(t)) return -1;
  if (++epoch == 0) {
    memset(stamp, 0, G.n * sizeof(uint32_t));
    epoch = 1;
  }
  heap.clear();
  dist[s] = 0;
  stamp[s] = epoch;
  heap.push(0, s);
  uint64_t d;
  uint32_t u;
  while (heap.pop(d, u)) {
    if (stamp[u] != epoch || dist[u] != d) continue;
    stamp[u] = epoch | kSettled;
    if (u == t) return static_cast<int64_t>(d);
    for (uint32_t i = G.fwd.ofs[u]; i < G.fwd.ofs[u + 1]; i++) {
      uint64_t ed = G.fwd.e[i];
      uint32_t v = static_cast<uint32_t>(ed);
      uint64_t nd = d + static_cast<uint32_t>(ed >> 32);
      uint32_t st = stamp[v];
      if (st == (epoch | kSettled)) continue;
      if ((st & ~kSettled) != epoch || nd < dist[v]) {
        stamp[v] = epoch;
        dist[v] = nd;
        heap.push(nd, v);
      }
    }
    for (int h = 0; h < G.nhuge; h++) {
      if (G.huge[h].u != u) continue;
      uint32_t v = G.huge[h].v;
      uint64_t nd = d + G.huge[h].w;
      uint32_t st = stamp[v];
      if (st == (epoch | kSettled)) continue;
      if ((st & ~kSettled) != epoch || nd < dist[v]) {
        stamp[v] = epoch;
        dist[v] = nd;
        heap.push(nd, v);
      }
    }
  }
  return -1;
}

void append_i64(std::vector<char>& o, int64_t x) {
  if (x < 0) {
    o.push_back('-');
    x = -x;
  }
  char tmp[24];
  int n = 0;
  uint64_t u = static_cast<uint64_t>(x);
  do {
    tmp[n++] = static_cast<char>('0' + static_cast<int>(u % 10));
    u /= 10;
  } while (u);
  while (n--) o.push_back(tmp[n]);
  o.push_back('\n');
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

void pin(int t) {
  cpu_set_t set;
  CPU_ZERO(&set);
  unsigned hc = std::thread::hardware_concurrency();
  if (!hc) hc = 1;
  CPU_SET(static_cast<unsigned>(t) % hc, &set);
  sched_setaffinity(0, sizeof(set), &set);
}

}  // namespace

int main() {
  bool prof = std::getenv("SD_PROF") != nullptr;
  double t0 = now_ms();
  std::string dir = env_or("DATA", "/data");
  std::string outp = std::string(env_or("OUT", "/out")) + "/distances.txt";
  int T = worker_count();

  uint32_t n = 0, m = 0;
  Edge* edges = parse_graph((dir + "/graph.txt").c_str(), n, m, T);
  double t_parse = now_ms();
  Graph g = build(edges, m, n);
  double t_build = now_ms();

  int qfd = ::open((dir + "/queries.txt").c_str(), O_RDONLY);
  if (qfd < 0) die("open queries");
  struct stat st;
  if (fstat(qfd, &st) != 0) die("fstat queries");
  size_t qlen = static_cast<size_t>(st.st_size);
  char* qbuf = map_arr<char>(qlen + 1);
  size_t got = 0;
  while (got < qlen) {
    ssize_t r = ::read(qfd, qbuf + got, qlen - got);
    if (r <= 0) die("read queries");
    got += static_cast<size_t>(r);
  }
  close(qfd);
  qbuf[qlen] = 0;
  std::vector<uint32_t> qs, qt;
  qs.reserve(2048);
  qt.reserve(2048);
  const char* qp = qbuf;
  const char* qe = qbuf + qlen;
  while (qp < qe && *qp) {
    uint32_t s = take(qp);
    uint32_t t = take(qp);
    if (g.o2n) {
      s = g.o2n[s];
      t = g.o2n[t];
    }
    qs.push_back(s);
    qt.push_back(t);
  }
  unmap(qbuf, qlen + 1, 1);
  if (g.o2n) {
    unmap(g.o2n, n, sizeof(uint32_t));
    g.o2n = nullptr;
  }

  int Q = static_cast<int>(qs.size());
  int TW = std::min(T, std::max(1, Q));
  std::vector<int64_t> ans(static_cast<size_t>(Q));
  std::atomic<int> next{0};
  std::vector<uint64_t> exps(static_cast<size_t>(TW));
  std::vector<std::thread> pool;
  for (int t = 0; t < TW; t++) {
    pool.emplace_back([&, t] {
      pin(t);
      Worker w;
      w.init(&g);
      for (;;) {
        int i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= Q) break;
        ans[static_cast<size_t>(i)] = w.search(qs[static_cast<size_t>(i)], qt[static_cast<size_t>(i)]);
      }
      exps[static_cast<size_t>(t)] = w.expanded;
    });
  }
  for (auto& th : pool) th.join();
  double t_query = now_ms();

  int nneed = 0;
  for (int i = 0; i < Q; i++)
    if (ans[static_cast<size_t>(i)] == kNeed) nneed++;
  if (nneed) {
    uint64_t* dist = map_arr<uint64_t>(n);
    uint32_t* stamp = map_arr<uint32_t>(n);
    memset(stamp, 0, n * sizeof(uint32_t));
    uint32_t epoch = 0;
    HeapW heap;
    for (int i = 0; i < Q; i++) {
      if (ans[static_cast<size_t>(i)] != kNeed) continue;
      ans[static_cast<size_t>(i)] =
          search64w(g, qs[static_cast<size_t>(i)], qt[static_cast<size_t>(i)], dist, stamp, epoch, heap);
    }
    unmap(dist, n, sizeof(uint64_t));
    unmap(stamp, n, sizeof(uint32_t));
  }
  double t_fall = now_ms();

  std::vector<char> out;
  out.reserve(static_cast<size_t>(Q) * 8);
  for (int64_t d : ans) append_i64(out, d);
  int ofd = ::open(outp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (ofd < 0) die("open output");
  write_all(ofd, out.data(), out.size());
  close(ofd);

  if (prof) {
    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    uint64_t exp = 0;
    for (uint64_t e : exps) exp += e;
    fprintf(stderr,
            "parse %.1f build %.1f query %.1f fall %.1f total %.1f ms | rss %ld KB | fm %u rm %u "
            "huge %d maxw %u need %d threads %d exp %llu\n",
            t_parse - t0, t_build - t_parse, t_query - t_build, t_fall - t_query, t_fall - t0, ru.ru_maxrss,
            g.fwd.m, g.rev.m, g.nhuge, g.max_small, nneed, TW, static_cast<unsigned long long>(exp));
  }
  g.release();
  return 0;
}
