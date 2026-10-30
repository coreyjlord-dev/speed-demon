// Track D (Traverse): minimum-weight CSR, bidirectional Dijkstra.
//
// Parallel edges collapse to the lightest one and self-loops are
// dropped; neither can change a shortest path. Queries run in
// parallel, one per P-core. Each worker keeps distance labels in
// epoch-stamped arrays so a query does not wipe 1.7M entries.
//
// Bidirectional search expands the side that has settled fewer nodes,
// which finishes an island or a dead-end before it walks the road
// network. It stops when the sum of the two heap minima is at least
// the best s-t walk found, or when either side runs out of nodes.
// s == t is 0. A source with no out-edge or a target with no in-edge
// is -1 without a search.
//
// The heap is 4-ary. Stale entries (a node pushed again at a better
// distance) are skipped on pop. Weights are non-negative, so the
// first time a node is popped at its current label it is settled.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr uint64_t kInf = ~0ull;

[[noreturn]] void die(const char* msg) {
  fprintf(stderr, "track-d: %s\n", msg);
  exit(1);
}

int worker_count() {
  if (const char* env = std::getenv("SD_THREADS")) {
    int n = std::atoi(env);
    if (n >= 1 && n <= 64) return n;
  }
  unsigned hc = std::thread::hardware_concurrency();
  if (hc == 0) return 1;
  // Dijkstra is cache-heavy. One worker per physical core.
  if (hc >= 8) return static_cast<int>(hc / 2);
  return static_cast<int>(hc);
}

struct Mapped {
  int fd = -1;
  size_t len = 0;
  const char* data = nullptr;

  explicit Mapped(const char* path) {
    fd = ::open(path, O_RDONLY);
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

inline uint32_t take_u32(const char*& p) {
  uint32_t x = static_cast<uint32_t>(*p++ - '0');
  while (*p >= '0' && *p <= '9') {
    x = x * 10u + static_cast<uint32_t>(*p++ - '0');
  }
  return x;
}

struct Graph {
  std::vector<uint32_t> ofs;
  std::vector<uint32_t> head;
  std::vector<uint32_t> wt;
};

void sort_small(std::pair<uint32_t, uint32_t>* a, std::pair<uint32_t, uint32_t>* b) {
  for (auto* i = a + 1; i < b; ++i) {
    auto key = *i;
    auto* j = i;
    while (j > a && (j[-1].first > key.first ||
                     (j[-1].first == key.first && j[-1].second > key.second))) {
      *j = j[-1];
      --j;
    }
    *j = key;
  }
}

// from -> to, minimum weight, self-loops removed.
void build_csr(const std::vector<uint32_t>& from, const std::vector<uint32_t>& to,
               const std::vector<uint32_t>& w, uint32_t n, Graph& g) {
  g.ofs.assign(n + 1, 0);
  for (uint32_t u : from) g.ofs[u + 1]++;
  for (uint32_t i = 1; i <= n; i++) g.ofs[i] += g.ofs[i - 1];
  std::vector<std::pair<uint32_t, uint32_t>> adj(from.size());
  std::vector<uint32_t> cur = g.ofs;
  for (size_t i = 0; i < from.size(); i++) {
    uint32_t p = cur[from[i]]++;
    adj[p] = {to[i], w[i]};
  }
  std::vector<std::pair<uint32_t, uint32_t>> packed;
  packed.reserve(from.size());
  uint32_t wri = 0;
  for (uint32_t u = 0; u < n; u++) {
    uint32_t a = g.ofs[u];
    uint32_t b = g.ofs[u + 1];
    if (b - a > 32) {
      std::sort(adj.begin() + a, adj.begin() + b);
    } else if (b > a) {
      sort_small(adj.data() + a, adj.data() + b);
    }
    g.ofs[u] = wri;
    for (uint32_t i = a; i < b;) {
      uint32_t v = adj[i].first;
      uint32_t wt = adj[i].second;
      uint32_t j = i + 1;
      while (j < b && adj[j].first == v) j++;
      if (v != u) {
        packed.push_back({v, wt});
        wri++;
      }
      i = j;
    }
  }
  g.ofs[n] = wri;
  g.head.resize(wri);
  g.wt.resize(wri);
  for (uint32_t i = 0; i < wri; i++) {
    g.head[i] = packed[i].first;
    g.wt[i] = packed[i].second;
  }
}

// Integer radix heap. Bucket 0 holds every label equal to `last`.
// Pushing a smaller label than `last` would break the invariant;
// Dijkstra never does that.
struct Heap {
  std::vector<std::pair<uint64_t, uint32_t>> b[65];
  uint64_t last = 0;
  int sz = 0;

  bool empty() const { return sz == 0; }

  int bucket(uint64_t d) const {
    if (d == last) return 0;
    return 64 - __builtin_clzll(d ^ last);
  }

  void clear() {
    for (int i = 0; i < 65; i++) b[i].clear();
    last = 0;
    sz = 0;
  }

  void push(uint64_t d, uint32_t v) {
    b[bucket(d)].emplace_back(d, v);
    sz++;
  }

  void refill() {
    int i = 1;
    while (b[i].empty()) i++;
    uint64_t m = ~0ull;
    for (const auto& x : b[i]) m = std::min(m, x.first);
    last = m;
    auto items = std::move(b[i]);
    b[i].clear();
    for (const auto& x : items) b[bucket(x.first)].push_back(x);
  }

  uint64_t min() {
    if (b[0].empty()) refill();
    return last;
  }

  std::pair<uint64_t, uint32_t> pop() {
    if (b[0].empty()) refill();
    auto r = b[0].back();
    b[0].pop_back();
    sz--;
    return r;
  }
};

struct Worker {
  const Graph* fwd;
  const Graph* rev;
  std::vector<uint64_t> df, db;
  std::vector<uint32_t> sf, sb, zf, zb;
  Heap hf, hb;
  uint32_t epoch = 1;
  uint32_t ns_f = 0;
  uint32_t ns_b = 0;
  uint64_t best = kInf;

  Worker(uint32_t n, const Graph* f, const Graph* r)
      : fwd(f), rev(r), df(n), db(n), sf(n), sb(n), zf(n), zb(n) {}

  void relax_from(bool forward, uint32_t u, uint64_t d) {
    const Graph* g = forward ? fwd : rev;
    auto& dist = forward ? df : db;
    auto& stamp = forward ? sf : sb;
    auto& other_stamp = forward ? sb : sf;
    auto& other_dist = forward ? db : df;
    Heap& heap = forward ? hf : hb;
    uint32_t a = g->ofs[u];
    uint32_t b = g->ofs[u + 1];
    for (uint32_t i = a; i < b; i++) {
      uint32_t v = g->head[i];
      uint64_t nd = d + g->wt[i];
      if (stamp[v] != epoch || nd < dist[v]) {
        stamp[v] = epoch;
        dist[v] = nd;
        heap.push(nd, v);
        if (other_stamp[v] == epoch) {
          uint64_t sum = nd + other_dist[v];
          if (sum < best) best = sum;
        }
      }
    }
  }

  void expand(bool forward) {
    Heap& heap = forward ? hf : hb;
    auto [d, u] = heap.pop();
    auto& settled = forward ? zf : zb;
    auto& stamp = forward ? sf : sb;
    auto& dist = forward ? df : db;
    if (settled[u] == epoch) return;
    if (stamp[u] != epoch || dist[u] != d) return;
    settled[u] = epoch;
    if (forward) ns_f++;
    else ns_b++;
    auto& other_stamp = forward ? sb : sf;
    auto& other_dist = forward ? db : df;
    if (other_stamp[u] == epoch) {
      uint64_t sum = d + other_dist[u];
      if (sum < best) best = sum;
    }
    relax_from(forward, u, d);
  }

  int64_t search(uint32_t s, uint32_t t) {
    if (s == t) return 0;
    if (fwd->ofs[s] == fwd->ofs[s + 1]) return -1;
    if (rev->ofs[t] == rev->ofs[t + 1]) return -1;
    if (++epoch == 0) {
      std::fill(sf.begin(), sf.end(), 0);
      std::fill(sb.begin(), sb.end(), 0);
      std::fill(zf.begin(), zf.end(), 0);
      std::fill(zb.begin(), zb.end(), 0);
      epoch = 1;
    }
    hf.clear();
    hb.clear();
    best = kInf;
    ns_f = ns_b = 0;
    sf[s] = epoch;
    df[s] = 0;
    hf.push(0, s);
    sb[t] = epoch;
    db[t] = 0;
    hb.push(0, t);
    while (!hf.empty() && !hb.empty()) {
      uint64_t mf = hf.min();
      uint64_t mb = hb.min();
      if (mf > kInf - mb || mf + mb >= best) break;
      if (ns_f <= ns_b) expand(true);
      else expand(false);
    }
    if (best == kInf) return -1;
    return static_cast<int64_t>(best);
  }
};

void append_i64(std::vector<char>& o, int64_t x) {
  if (x < 0) {
    o.push_back('-');
    x = -x;
  }
  char tmp[24];
  int n = 0;
  uint64_t u = static_cast<uint64_t>(x);
  do {
    tmp[n++] = static_cast<char>('0' + u % 10);
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

}  // namespace

int main() {
  std::string dir = env_or("DATA", "/data");
  std::string outp = std::string(env_or("OUT", "/out")) + "/distances.txt";
  Mapped graph_file((dir + "/graph.txt").c_str());
  Mapped query_file((dir + "/queries.txt").c_str());

  const char* p = graph_file.data;
  uint32_t n = take_u32(p);
  p++;
  uint32_t m = take_u32(p);
  p++;
  std::vector<uint32_t> U(m), V(m), W(m);
  for (uint32_t i = 0; i < m; i++) {
    U[i] = take_u32(p);
    p++;
    V[i] = take_u32(p);
    p++;
    W[i] = take_u32(p);
    p++;
  }

  Graph fwd, rev;
  build_csr(U, V, W, n, fwd);
  build_csr(V, U, W, n, rev);
  std::vector<uint32_t>().swap(U);
  std::vector<uint32_t>().swap(V);
  std::vector<uint32_t>().swap(W);

  std::vector<std::pair<uint32_t, uint32_t>> queries;
  const char* q = query_file.data;
  const char* qend = query_file.data + query_file.len;
  while (q < qend) {
    uint32_t s = take_u32(q);
    q++;
    uint32_t t = take_u32(q);
    if (q < qend && *q == '\n') q++;
    queries.emplace_back(s, t);
  }

  int Q = static_cast<int>(queries.size());
  std::vector<int64_t> ans(queries.size());
  int T = std::min(worker_count(), std::max(1, Q));
  std::atomic<int> next{0};
  std::vector<std::thread> pool;
  pool.reserve(static_cast<size_t>(T));
  for (int t = 0; t < T; t++) {
    pool.emplace_back([&, n] {
      Worker w(n, &fwd, &rev);
      for (;;) {
        int i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= Q) break;
        ans[i] = w.search(queries[i].first, queries[i].second);
      }
    });
  }
  for (auto& th : pool) th.join();

  std::vector<char> out;
  out.reserve(queries.size() * 8);
  for (int64_t d : ans) append_i64(out, d);
  int fd = ::open(outp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) die("open output");
  write_all(fd, out.data(), out.size());
  close(fd);
  return 0;
}
