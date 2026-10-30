// One forward CSR, BFS renumber, radix heap, distances in recycled pages.

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

constexpr int kPage = 512;
constexpr int kPool = 4096;
constexpr uint64_t kInf = ~0ull;

[[noreturn]] void die(const char *m) {
  fprintf(stderr, "track-d: %s\n", m);
  exit(1);
}

int threads() {
  if (const char *e = std::getenv("SD_THREADS")) {
    int n = std::atoi(e);
    if (n >= 1 && n <= 64)
      return n;
  }
  unsigned h = std::thread::hardware_concurrency();
  if (h >= 12)
    return 6;
  return h ? (int)h : 1;
}

template <class T> T *mapn(size_t n) {
  if (!n)
    n = 1;
  void *p = mmap(nullptr, n * sizeof(T), PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (p == MAP_FAILED)
    die("mmap");
  return (T *)p;
}

void unmap(void *p, size_t n, size_t z) {
  if (p)
    munmap(p, (n ? n : 1) * z);
}

struct Graph {
  uint32_t n = 0, m = 0;
  uint32_t *ofs = nullptr, *hd = nullptr, *wt = nullptr;
  uint32_t *inb = nullptr;
  bool has_in(uint32_t v) const { return (inb[v >> 5] >> (v & 31)) & 1u; }
};

struct Store {
  uint64_t *dist[4096]{};
  uint16_t *stamp[4096]{};
  int live[kPool]{};
  int nlive = 0, used = 0, epoch = 1;
  uint64_t *ds = nullptr;
  uint16_t *ss = nullptr;
  void init() {
    ds = mapn<uint64_t>(kPool * kPage);
    ss = mapn<uint16_t>(kPool * kPage);
  }
  void reset() {
    for (int i = 0; i < nlive; i++) {
      dist[live[i]] = nullptr;
      stamp[live[i]] = nullptr;
    }
    nlive = used = 0;
    if (++epoch == 0)
      epoch = 1;
  }
  void touch(uint32_t c) {
    if (dist[c])
      return;
    if (used == kPool)
      die("pages");
    dist[c] = ds + used * kPage;
    stamp[c] = ss + used * kPage;
    live[nlive++] = (int)c;
    ++used;
  }
  // Returns current distance, or kInf if this query has not labeled v.
  uint64_t get(uint32_t v) {
    uint32_t c = v >> 9, o = v & 511;
    if (!dist[c])
      return kInf;
    if (stamp[c][o] != (uint16_t)epoch)
      return kInf;
    return dist[c][o];
  }
  void set(uint32_t v, uint64_t d) {
    uint32_t c = v >> 9, o = v & 511;
    touch(c);
    stamp[c][o] = (uint16_t)epoch;
    dist[c][o] = d;
  }
};

struct Heap {
  uint64_t last = 0;
  int sz = 0;
  uint32_t *nv[65]{};
  uint64_t *dv[65]{};
  int len[65]{}, cap[65]{};
  void clear() {
    for (int i = 0; i < 65; i++)
      len[i] = 0;
    last = sz = 0;
  }
  void push(uint64_t d, uint32_t v) {
    int i = d == last ? 0 : 64 - __builtin_clzll(d ^ last);
    if (len[i] == cap[i]) {
      int nc = cap[i] ? cap[i] * 2 : 8;
      nv[i] = (uint32_t *)realloc(nv[i], (size_t)nc * 4);
      dv[i] = (uint64_t *)realloc(dv[i], (size_t)nc * 8);
      if (!nv[i] || !dv[i])
        die("realloc");
      cap[i] = nc;
    }
    nv[i][len[i]] = v;
    dv[i][len[i]++] = d;
    ++sz;
  }
  void refill() {
    int i = 1;
    while (i < 65 && !len[i])
      ++i;
    uint64_t m = kInf;
    int L = len[i];
    for (int k = 0; k < L; k++)
      if (dv[i][k] < m)
        m = dv[i][k];
    last = m;
    uint32_t *vn = (uint32_t *)malloc((size_t)L * 4);
    uint64_t *vd = (uint64_t *)malloc((size_t)L * 8);
    if (!vn || !vd)
      die("malloc");
    memcpy(vn, nv[i], (size_t)L * 4);
    memcpy(vd, dv[i], (size_t)L * 8);
    len[i] = 0;
    sz -= L;
    for (int k = 0; k < L; k++)
      push(vd[k], vn[k]);
    free(vn);
    free(vd);
  }
  bool pop(uint64_t &d, uint32_t &v) {
    if (!sz)
      return false;
    if (!len[0])
      refill();
    d = dv[0][--len[0]];
    v = nv[0][len[0]];
    --sz;
    return true;
  }
};

int64_t dijkstra(const Graph &g, Store &st, Heap &h, uint32_t s, uint32_t t) {
  if (s == t)
    return 0;
  if (g.ofs[s] == g.ofs[s + 1])
    return -1;
  if (!g.has_in(t))
    return -1;
  st.reset();
  h.clear();
  st.set(s, 0);
  h.push(0, s);
  uint64_t d;
  uint32_t u;
  while (h.pop(d, u)) {
    if (st.get(u) != d)
      continue;
    if (u == t)
      return (int64_t)d;
    for (uint32_t i = g.ofs[u]; i < g.ofs[u + 1]; i++) {
      uint32_t v = g.hd[i];
      uint64_t nd = d + g.wt[i];
      uint32_t c = v >> 9, o = v & 511;
      if (!st.dist[c])
        st.touch(c);
      if (st.stamp[c][o] != (uint16_t)st.epoch || nd < st.dist[c][o]) {
        st.stamp[c][o] = (uint16_t)st.epoch;
        st.dist[c][o] = nd;
        h.push(nd, v);
      }
    }
  }
  return -1;
}

struct Scan {
  int fd;
  off_t pos, end;
  char buf[1 << 16];
  int n = 0, i = 0;
  explicit Scan(int fd, off_t b, off_t e) : fd(fd), pos(b), end(e) {}
  void fill() {
    int left = n - i;
    if (left && i)
      memmove(buf, buf + i, left);
    size_t room = sizeof(buf) - 1 - (left > 0 ? left : 0);
    off_t want = end - pos;
    if ((off_t)room > want)
      room = (size_t)want;
    ssize_t r = room ? ::pread(fd, buf + (left > 0 ? left : 0), room, pos) : 0;
    if (r < 0)
      die("pread");
    pos += r;
    n = (left > 0 ? left : 0) + (int)r;
    buf[n] = 0;
    i = 0;
  }
  bool num(uint32_t &o) {
    if (i + 24 >= n)
      fill();
    if (i >= n)
      return false;
    if ((unsigned char)buf[i] < '0')
      return false;
    uint32_t x = 0;
    unsigned char c;
    while ((c = (unsigned char)buf[i]) >= '0') {
      x = x * 10u + c - '0';
      ++i;
    }
    if (c)
      ++i;
    o = x;
    return true;
  }
};

int g_fd = -1;
off_t g_edge = 0, g_end = 0;
uint32_t *g_deg = nullptr, *g_cur = nullptr, *g_hd = nullptr, *g_wt = nullptr,
         *g_inb = nullptr;
void on_place(uint32_t u, uint32_t v, uint32_t w);

void scan_range(off_t b, off_t e, int pass) {
  if (b >= e)
    return;
  Scan sc(g_fd, b, e);
  if (b != g_edge) {
    while (sc.i >= sc.n) {
      sc.fill();
      if (sc.i >= sc.n)
        return;
    }
    while (sc.i < sc.n && sc.buf[sc.i] != '\n')
      ++sc.i;
    if (sc.i < sc.n)
      ++sc.i;
  }
  uint32_t u, v, w;
  while (sc.num(u) && sc.num(v) && sc.num(w)) {
    if (u == v)
      continue;
    if (pass == 0) {
      __atomic_fetch_add(g_deg + u + 1, 1, __ATOMIC_RELAXED);
      __atomic_fetch_or(g_inb + (v >> 5), 1u << (v & 31), __ATOMIC_RELAXED);
    } else {
      on_place(u, v, w);
    }
  }
}

void parallel_scan(int pass) {
  int T = threads();
  if (T > 8)
    T = 8;
  if (g_end - g_edge < (1 << 20))
    T = 1;
  std::vector<off_t> cut((size_t)T + 1);
  cut[0] = g_edge;
  cut[(size_t)T] = g_end;
  off_t span = g_end - g_edge;
  for (int t = 1; t < T; t++) {
    off_t x = g_edge + span * t / T;
    char b[96];
    ssize_t r = ::pread(g_fd, b, 95, x);
    int k = 0;
    while (k < r && b[k] != '\n')
      ++k;
    cut[(size_t)t] = x + (k < r ? k + 1 : 0);
    if (cut[(size_t)t] < cut[(size_t)t - 1])
      cut[(size_t)t] = cut[(size_t)t - 1];
  }
  std::vector<std::thread> pool;
  for (int t = 0; t < T; t++)
    pool.emplace_back(scan_range, cut[(size_t)t], cut[(size_t)t + 1], pass);
  for (auto &th : pool)
    th.join();
}

void on_place(uint32_t u, uint32_t v, uint32_t w) {
  uint32_t p = __atomic_fetch_add(g_cur + u, 1, __ATOMIC_RELAXED);
  g_hd[p] = v;
  g_wt[p] = w;
}

void renumber(Graph &g, uint32_t *o2n) {
  uint32_t n = g.n;
  uint8_t *seen = mapn<uint8_t>(n);
  memset(seen, 0, n);
  uint32_t *q = mapn<uint32_t>(n);
  uint32_t nid = 0, qh = 0, qt = 0;
  for (uint32_t s = 0; s < n; s++) {
    if (seen[s])
      continue;
    seen[s] = 1;
    q[qt++] = s;
    while (qh < qt) {
      uint32_t u = q[qh++];
      o2n[u] = nid++;
      for (uint32_t i = g.ofs[u]; i < g.ofs[u + 1]; i++) {
        uint32_t v = g.hd[i];
        if (seen[v])
          continue;
        seen[v] = 1;
        q[qt++] = v;
      }
    }
  }
  unmap(seen, n, 1);
  unmap(q, n, 4);
  uint32_t words = (n + 31) / 32 + 1;
  memset(g.inb, 0, words * 4);
  uint32_t *nofs = mapn<uint32_t>(n + 1);
  memset(nofs, 0, (n + 1ull) * 4);
  for (uint32_t u = 0; u < n; u++)
    nofs[o2n[u] + 1] += g.ofs[u + 1] - g.ofs[u];
  for (uint32_t i = 1; i <= n; i++)
    nofs[i] += nofs[i - 1];
  uint32_t m = g.m;
  uint32_t *nwt = mapn<uint32_t>(m ? m : 1);
  uint32_t *cur = mapn<uint32_t>(n);
  memcpy(cur, nofs, n * 4);
  for (uint32_t u = 0; u < n; u++) {
    uint32_t nu = o2n[u];
    for (uint32_t i = g.ofs[u]; i < g.ofs[u + 1]; i++)
      nwt[cur[nu]++] = g.wt[i];
  }
  unmap(g.wt, m ? m : 1, 4);
  memcpy(cur, nofs, n * 4);
  uint32_t *nhd = mapn<uint32_t>(m ? m : 1);
  for (uint32_t u = 0; u < n; u++) {
    uint32_t nu = o2n[u];
    for (uint32_t i = g.ofs[u]; i < g.ofs[u + 1]; i++) {
      uint32_t v = o2n[g.hd[i]];
      nhd[cur[nu]++] = v;
      g.inb[v >> 5] |= 1u << (v & 31);
    }
  }
  unmap(g.hd, m ? m : 1, 4);
  unmap(cur, n, 4);
  unmap(g.ofs, n + 1, 4);
  g.ofs = nofs;
  g.hd = nhd;
  g.wt = nwt;
}

void load_graph(const char *path, Graph &g, uint32_t *&o2n) {
  int fd = ::open(path, O_RDONLY);
  if (fd < 0)
    die("open graph");
  struct stat st;
  if (fstat(fd, &st))
    die("fstat");
  char hdr[64];
  ssize_t hr = ::pread(fd, hdr, 63, 0);
  if (hr < 2)
    die("header");
  hdr[hr] = 0;
  char *hp = hdr;
  auto hnum = [&]() {
    uint32_t x = 0;
    while (*hp >= '0')
      x = x * 10u + (uint32_t)(*hp++ - '0');
    if (*hp)
      ++hp;
    return x;
  };
  g.n = hnum();
  g.m = hnum();
  off_t edge = (off_t)(hp - hdr);
  while (edge > 0 && hdr[edge - 1] != '\n')
    --edge;
  // hnum consumed the header in memory; the file offset of the first edge is
  // the first newline + 1.
  edge = 0;
  while (hdr[edge] && hdr[edge] != '\n')
    ++edge;
  ++edge;
  g_fd = fd;
  g_edge = edge;
  g_end = st.st_size;

  g_deg = mapn<uint32_t>(g.n + 1);
  memset(g_deg, 0, (g.n + 1ull) * 4);
  g.inb = g_inb = mapn<uint32_t>((g.n + 31) / 32 + 1);
  memset(g.inb, 0, ((g.n + 31) / 32 + 1ull) * 4);
  parallel_scan(0);
  for (uint32_t i = 1; i <= g.n; i++)
    g_deg[i] += g_deg[i - 1];
  g.m = g_deg[g.n];
  g.ofs = g_deg;
  g.hd = mapn<uint32_t>(g.m ? g.m : 1);
  g.wt = mapn<uint32_t>(g.m ? g.m : 1);
  g_cur = mapn<uint32_t>(g.n);
  memcpy(g_cur, g.ofs, g.n * 4);
  g_hd = g.hd;
  g_wt = g.wt;
  parallel_scan(1);
  unmap(g_cur, g.n, 4);
  close(fd);

  o2n = mapn<uint32_t>(g.n);
  renumber(g, o2n);
}

void append_i64(char *&o, int64_t x) {
  if (x < 0) {
    *o++ = '-';
    x = -x;
  }
  char tmp[24];
  int n = 0;
  uint64_t u = (uint64_t)x;
  do
    tmp[n++] = (char)('0' + u % 10);
  while (u /= 10);
  while (n--)
    *o++ = tmp[n];
  *o++ = '\n';
}

const char *env_or(const char *k, const char *d) {
  const char *v = std::getenv(k);
  return v && *v ? v : d;
}

} // namespace

int main() {
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
  mallopt(M_MMAP_THRESHOLD, 64 * 1024);
  mallopt(M_TRIM_THRESHOLD, 64 * 1024);

  std::string dir = env_or("DATA", "/data");
  std::string outp = std::string(env_or("OUT", "/out")) + "/distances.txt";
  Graph g;
  uint32_t *o2n = nullptr;
  load_graph((dir + "/graph.txt").c_str(), g, o2n);

  int qfd = ::open((dir + "/queries.txt").c_str(), O_RDONLY);
  if (qfd < 0)
    die("open queries");
  struct stat st;
  if (fstat(qfd, &st))
    die("fstat q");
  char *qb = mapn<char>((size_t)st.st_size + 1);
  size_t got = 0;
  while (got < (size_t)st.st_size) {
    ssize_t r = ::read(qfd, qb + got, (size_t)st.st_size - got);
    if (r <= 0)
      die("read q");
    got += (size_t)r;
  }
  close(qfd);
  qb[got] = 0;
  std::vector<uint32_t> qs, qt;
  for (char *p = qb; *p;) {
    uint32_t s = 0, t = 0;
    while (*p >= '0')
      s = s * 10u + (uint32_t)(*p++ - '0');
    if (*p)
      ++p;
    while (*p >= '0')
      t = t * 10u + (uint32_t)(*p++ - '0');
    if (*p)
      ++p;
    qs.push_back(o2n[s]);
    qt.push_back(o2n[t]);
  }
  unmap(qb, (size_t)st.st_size + 1, 1);
  unmap(o2n, g.n, 4);

  int Q = (int)qs.size();
  int T = threads();
  if (T > Q)
    T = Q;
  if (T < 1)
    T = 1;
  std::vector<int64_t> ans(Q);
  std::atomic<int> next{0};
  std::vector<std::thread> pool;
  for (int t = 0; t < T; t++) {
    pool.emplace_back([&] {
      Store st;
      st.init();
      Heap h;
      for (;;) {
        int i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= Q)
          break;
        ans[i] = dijkstra(g, st, h, qs[i], qt[i]);
      }
    });
  }
  for (auto &th : pool)
    th.join();

  char *out = mapn<char>((size_t)Q * 24 + 8);
  char *op = out;
  for (int64_t d : ans)
    append_i64(op, d);
  int ofd = ::open(outp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (ofd < 0)
    die("open out");
  size_t left = (size_t)(op - out);
  const char *wp = out;
  while (left) {
    ssize_t k = ::write(ofd, wp, left);
    if (k < 0)
      die("write");
    wp += k;
    left -= (size_t)k;
  }
  close(ofd);
  return 0;
}
