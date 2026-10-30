// Track B (Index): index only the terms the queries name.
//
// Queries are parsed first. BOOL terms get a sorted document list
// (a bitset once the list is wide). PHRASE terms also store token
// positions. The corpus is mapped and tokenized in parallel; a token
// is a maximal run of [a-z0-9] after ASCII lowercasing, and every
// other byte, including UTF-8, is a separator. Workers own disjoint
// doc-id ranges, so posting lists concatenate in order.
//
// BOOL is left to right with no precedence. AND/OR/NOT run on the
// sorted lists or the bitsets, whichever is smaller. A phrase matches
// when the rarest term's positions have the other terms at the
// neighbouring offsets. Documents are counted once.
//
// Answers are count, sum of ids, and the ten smallest ids.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr uint32_t kEmpty = 0xffffffffu;
constexpr uint32_t kDense = 8192;
constexpr uint8_t kAnd = 0;
constexpr uint8_t kOr = 1;
constexpr uint8_t kNot = 2;

[[noreturn]] void die(const char* msg) {
  fprintf(stderr, "track-b: %s\n", msg);
  exit(1);
}

int worker_count() {
  if (const char* env = std::getenv("SD_THREADS")) {
    int n = std::atoi(env);
    if (n >= 1 && n <= 64) return n;
  }
  unsigned hc = std::thread::hardware_concurrency();
  if (hc == 0) return 1;
  if (hc >= 8) return static_cast<int>(hc / 2);
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

uint64_t hash_tok(const char* s, uint32_t n) {
  // OR 0x20 lowercases A-Z and leaves 0-9 unchanged.
  uint64_t h = 0x9E3779B185EBCA87ull ^ (static_cast<uint64_t>(n) * 0xC2B2AE3D27D4EB4Full);
  for (uint32_t i = 0; i < n; i++) {
    h ^= static_cast<unsigned char>(s[i]) | 0x20u;
    h *= 0x100000001B3ull;
  }
  return h ^ (h >> 33);
}

bool eq_tok(const char* stored, const char* s, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) {
    if (static_cast<unsigned char>(stored[i]) != (static_cast<unsigned char>(s[i]) | 0x20u)) {
      return false;
    }
  }
  return true;
}

struct Dict {
  struct Slot {
    uint64_t hash = 0;
    uint32_t off = 0;
    uint32_t id = kEmpty;
    uint16_t len = 0;
  };
  std::string arena;
  std::vector<Slot> slots;
  std::vector<char> need_pos;
  uint32_t mask = 0;
  uint32_t used = 0;
  uint32_t nterms = 0;

  Dict() { rehash(4096); }

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

  // Insert a query term (already lowercase). Returns its id.
  uint32_t add(const char* s, uint32_t n) {
    uint64_t h = hash_tok(s, n);
    if ((used + 1) * 10 >= (mask + 1) * 7) rehash((mask + 1) * 2);
    uint32_t i = static_cast<uint32_t>(h) & mask;
    for (;;) {
      Slot& sl = slots[i];
      if (sl.id == kEmpty) {
        sl.hash = h;
        sl.off = static_cast<uint32_t>(arena.size());
        sl.len = static_cast<uint16_t>(n);
        sl.id = nterms++;
        arena.append(s, n);
        need_pos.push_back(0);
        used++;
        return sl.id;
      }
      if (sl.hash == h && sl.len == n &&
          std::memcmp(arena.data() + sl.off, s, n) == 0) {
        return sl.id;
      }
      i = (i + 1) & mask;
    }
  }

  int32_t find(const char* s, uint32_t n) const {
    if (n == 0 || n > 32) return -1;
    uint64_t h = hash_tok(s, n);
    uint32_t i = static_cast<uint32_t>(h) & mask;
    for (;;) {
      const Slot& sl = slots[i];
      if (sl.id == kEmpty) return -1;
      if (sl.hash == h && sl.len == n && eq_tok(arena.data() + sl.off, s, n)) {
        return static_cast<int32_t>(sl.id);
      }
      i = (i + 1) & mask;
    }
  }
};

struct Query {
  bool phrase = false;
  uint32_t begin = 0;
  uint16_t n = 0;
  uint32_t op_begin = 0;
};

struct Post {
  std::vector<uint32_t> docs;
  std::vector<uint32_t> pos;
  std::vector<uint32_t> pofs;
  std::vector<uint64_t> bits;
};

struct Arena32 {
  std::vector<uint32_t*> blocks;
  uint32_t* cur = nullptr;
  uint32_t left = 0;

  Arena32() = default;
  Arena32(const Arena32&) = delete;
  Arena32& operator=(const Arena32&) = delete;
  Arena32(Arena32&& o) noexcept : blocks(std::move(o.blocks)), cur(o.cur), left(o.left) {
    o.cur = nullptr;
    o.left = 0;
  }
  Arena32& operator=(Arena32&& o) noexcept {
    if (this != &o) {
      for (uint32_t* b : blocks) delete[] b;
      blocks = std::move(o.blocks);
      cur = o.cur;
      left = o.left;
      o.cur = nullptr;
      o.left = 0;
    }
    return *this;
  }
  ~Arena32() {
    for (uint32_t* b : blocks) delete[] b;
  }

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
};

struct List {
  uint32_t* p = nullptr;
  uint32_t n = 0;
  uint32_t cap = 0;
};

inline void push_list(Arena32& a, List& L, uint32_t v) {
  if (L.n == L.cap) {
    uint32_t nc = L.cap ? L.cap * 2 : 4;
    uint32_t* np = a.alloc(nc);
    if (L.n) std::memcpy(np, L.p, L.n * sizeof(uint32_t));
    L.p = np;
    L.cap = nc;
  }
  L.p[L.n++] = v;
}

struct Local {
  Arena32 arena;
  std::vector<List> docs;
  std::vector<List> counts;
  std::vector<List> pos;
  std::vector<uint32_t> last;
  uint32_t max_id = 0;

  explicit Local(uint32_t nterms)
      : docs(nterms), counts(nterms), pos(nterms), last(nterms, kEmpty) {}
  Local(const Local&) = delete;
  Local& operator=(const Local&) = delete;
  Local(Local&&) = default;
  Local& operator=(Local&&) = default;
};

struct Ans {
  uint32_t count = 0;
  uint64_t sum = 0;
  uint32_t top[10]{};
  uint8_t nt = 0;
};

bool token_char(unsigned char c) {
  unsigned char f = c | 0x20u;
  return (f >= 'a' && f <= 'z') || (c >= '0' && c <= '9');
}

void on_token(Local& L, const Dict& dict, const char* s, uint32_t n, uint32_t doc, uint32_t pos) {
  int32_t id = dict.find(s, n);
  if (id < 0) return;
  uint32_t tid = static_cast<uint32_t>(id);
  if (L.last[tid] != doc) {
    L.last[tid] = doc;
    push_list(L.arena, L.docs[tid], doc);
    if (dict.need_pos[tid]) push_list(L.arena, L.counts[tid], 0);
  }
  if (dict.need_pos[tid]) {
    push_list(L.arena, L.pos[tid], pos);
    L.counts[tid].p[L.counts[tid].n - 1]++;
  }
}

void tokenize(Local& L, const Dict& dict, const char* text, uint32_t len, uint32_t doc) {
  uint32_t i = 0;
  uint32_t pos = 0;
  while (i < len) {
    if (!token_char(static_cast<unsigned char>(text[i]))) {
      i++;
      continue;
    }
    uint32_t st = i++;
    while (i < len && token_char(static_cast<unsigned char>(text[i]))) i++;
    on_token(L, dict, text + st, i - st, doc, pos);
    pos++;
  }
}

void index_range(const char* p, const char* limit, const Dict& dict, Local& L) {
  while (p < limit) {
    uint32_t id = 0;
    while (*p >= '0' && *p <= '9') {
      id = id * 10u + static_cast<uint32_t>(*p - '0');
      p++;
    }
    p++;  // tab
    const char* text = p;
    while (*p != '\n') p++;
    tokenize(L, dict, text, static_cast<uint32_t>(p - text), id);
    p++;
    if (id + 1 > L.max_id) L.max_id = id + 1;
  }
}

bool has_pos(const Post& p, uint32_t doc, uint32_t position) {
  auto it = std::lower_bound(p.docs.begin(), p.docs.end(), doc);
  if (it == p.docs.end() || *it != doc) return false;
  uint32_t i = static_cast<uint32_t>(it - p.docs.begin());
  uint32_t a = p.pofs[i];
  uint32_t b = p.pofs[i + 1];
  return std::binary_search(p.pos.begin() + a, p.pos.begin() + b, position);
}

bool bit_test(const std::vector<uint64_t>& b, uint32_t id) {
  size_t w = id >> 6;
  if (w >= b.size()) return false;
  return (b[w] >> (id & 63)) & 1ull;
}

void bit_set(std::vector<uint64_t>& b, uint32_t id) {
  b[id >> 6] |= 1ull << (id & 63);
}

void bit_clear(std::vector<uint64_t>& b, uint32_t id) {
  size_t w = id >> 6;
  if (w < b.size()) b[w] &= ~(1ull << (id & 63));
}

struct Cur {
  bool dense = false;
  std::vector<uint32_t> ids;
  std::vector<uint64_t> bits;
  std::vector<uint32_t> scratch;
  uint32_t words = 0;

  void clear_sparse() {
    dense = false;
    ids.clear();
  }

  void load(const Post& p) {
    if (!p.bits.empty()) {
      dense = true;
      if (bits.size() != p.bits.size()) bits.resize(p.bits.size());
      std::memcpy(bits.data(), p.bits.data(), p.bits.size() * sizeof(uint64_t));
      words = static_cast<uint32_t>(p.bits.size());
      return;
    }
    dense = false;
    ids.assign(p.docs.begin(), p.docs.end());
  }

  void to_dense(uint32_t nwords) {
    if (bits.size() != nwords) bits.assign(nwords, 0);
    else std::fill(bits.begin(), bits.end(), 0);
    for (uint32_t id : ids) bit_set(bits, id);
    dense = true;
    words = nwords;
  }
};

void and_sparse(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
                std::vector<uint32_t>& out) {
  out.clear();
  if (a.size() * 16 < b.size()) {
    out.reserve(a.size());
    for (uint32_t x : a) {
      if (std::binary_search(b.begin(), b.end(), x)) out.push_back(x);
    }
    return;
  }
  if (b.size() * 16 < a.size()) {
    out.reserve(b.size());
    for (uint32_t x : b) {
      if (std::binary_search(a.begin(), a.end(), x)) out.push_back(x);
    }
    return;
  }
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i] == b[j]) {
      out.push_back(a[i]);
      i++;
      j++;
    } else if (a[i] < b[j]) {
      i++;
    } else {
      j++;
    }
  }
}

void or_sparse(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
               std::vector<uint32_t>& out) {
  out.clear();
  out.reserve(a.size() + b.size());
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i] == b[j]) {
      out.push_back(a[i]);
      i++;
      j++;
    } else if (a[i] < b[j]) {
      out.push_back(a[i++]);
    } else {
      out.push_back(b[j++]);
    }
  }
  while (i < a.size()) out.push_back(a[i++]);
  while (j < b.size()) out.push_back(b[j++]);
}

void sub_sparse(const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
                std::vector<uint32_t>& out) {
  out.clear();
  size_t i = 0, j = 0;
  while (i < a.size() && j < b.size()) {
    if (a[i] == b[j]) {
      i++;
      j++;
    } else if (a[i] < b[j]) {
      out.push_back(a[i++]);
    } else {
      j++;
    }
  }
  while (i < a.size()) out.push_back(a[i++]);
}

void do_and(Cur& c, const Post& p) {
  if (!c.dense && c.ids.empty()) return;
  if (p.docs.empty()) {
    c.clear_sparse();
    return;
  }
  if (!c.dense && p.bits.empty()) {
    and_sparse(c.ids, p.docs, c.scratch);
    c.ids.swap(c.scratch);
    return;
  }
  if (c.dense && !p.bits.empty()) {
    size_t n = std::min(c.bits.size(), p.bits.size());
    for (size_t i = 0; i < n; i++) c.bits[i] &= p.bits[i];
    for (size_t i = n; i < c.bits.size(); i++) c.bits[i] = 0;
    return;
  }
  if (c.dense) {
    c.scratch.clear();
    for (uint32_t d : p.docs) {
      if (bit_test(c.bits, d)) c.scratch.push_back(d);
    }
    c.dense = false;
    c.ids.swap(c.scratch);
    return;
  }
  // Sparse current, dense term.
  size_t w = 0;
  for (uint32_t id : c.ids) {
    if (bit_test(p.bits, id)) c.ids[w++] = id;
  }
  c.ids.resize(w);
}

void do_or(Cur& c, const Post& p, uint32_t nwords) {
  if (p.docs.empty()) return;
  if (!c.dense && c.ids.empty()) {
    c.load(p);
    return;
  }
  if (c.dense && !p.bits.empty()) {
    for (size_t i = 0; i < p.bits.size(); i++) c.bits[i] |= p.bits[i];
    return;
  }
  if (c.dense) {
    for (uint32_t d : p.docs) bit_set(c.bits, d);
    return;
  }
  if (!p.bits.empty()) {
    c.to_dense(nwords);
    for (size_t i = 0; i < p.bits.size(); i++) c.bits[i] |= p.bits[i];
    return;
  }
  or_sparse(c.ids, p.docs, c.scratch);
  if (c.scratch.size() >= kDense) {
    c.ids.swap(c.scratch);
    c.to_dense(nwords);
  } else {
    c.dense = false;
    c.ids.swap(c.scratch);
  }
}

void do_not(Cur& c, const Post& p) {
  if (!c.dense && c.ids.empty()) return;
  if (p.docs.empty()) return;
  if (!c.dense && p.bits.empty()) {
    sub_sparse(c.ids, p.docs, c.scratch);
    c.ids.swap(c.scratch);
    return;
  }
  if (!c.dense) {
    size_t w = 0;
    for (uint32_t id : c.ids) {
      if (!bit_test(p.bits, id)) c.ids[w++] = id;
    }
    c.ids.resize(w);
    return;
  }
  if (p.bits.empty()) {
    for (uint32_t d : p.docs) bit_clear(c.bits, d);
    return;
  }
  size_t n = std::min(c.bits.size(), p.bits.size());
  for (size_t i = 0; i < n; i++) c.bits[i] &= ~p.bits[i];
}

Ans from_ids(const std::vector<uint32_t>& ids) {
  Ans a;
  a.count = static_cast<uint32_t>(ids.size());
  a.nt = static_cast<uint8_t>(std::min<size_t>(10, ids.size()));
  for (uint8_t i = 0; i < a.nt; i++) a.top[i] = ids[i];
  uint64_t sum = 0;
  for (uint32_t id : ids) sum += id;
  a.sum = sum;
  return a;
}

Ans from_bits(const std::vector<uint64_t>& bits, uint32_t N) {
  Ans a;
  for (size_t w = 0; w < bits.size(); w++) {
    uint64_t x = bits[w];
    while (x) {
      uint32_t b = static_cast<uint32_t>(__builtin_ctzll(x));
      uint32_t id = static_cast<uint32_t>(w * 64 + b);
      if (id >= N) break;
      if (a.nt < 10) a.top[a.nt++] = id;
      a.sum += id;
      a.count++;
      x &= x - 1;
    }
  }
  return a;
}

Ans eval_bool(const Query& q, const std::vector<uint32_t>& terms, const std::vector<uint8_t>& ops,
              const std::vector<Post>& posts, Cur& c, uint32_t N, uint32_t nwords) {
  c.load(posts[terms[q.begin]]);
  for (uint16_t i = 1; i < q.n; i++) {
    uint8_t op = ops[q.op_begin + (i - 1)];
    const Post& p = posts[terms[q.begin + i]];
    if (op == kAnd) do_and(c, p);
    else if (op == kOr) do_or(c, p, nwords);
    else do_not(c, p);
  }
  if (!c.dense && c.ids.empty()) return Ans{};
  if (c.dense) return from_bits(c.bits, N);
  return from_ids(c.ids);
}

Ans eval_phrase(const Query& q, const std::vector<uint32_t>& terms, const std::vector<Post>& posts,
                std::vector<uint32_t>& hits) {
  hits.clear();
  uint16_t n = q.n;
  for (uint16_t i = 0; i < n; i++) {
    if (posts[terms[q.begin + i]].docs.empty()) return Ans{};
  }
  uint16_t rare = 0;
  size_t best = posts[terms[q.begin]].pos.size();
  for (uint16_t i = 1; i < n; i++) {
    size_t sz = posts[terms[q.begin + i]].pos.size();
    if (sz < best) {
      best = sz;
      rare = i;
    }
  }
  const Post& rp = posts[terms[q.begin + rare]];
  for (uint32_t di = 0; di < rp.docs.size(); di++) {
    uint32_t doc = rp.docs[di];
    uint32_t a = rp.pofs[di];
    uint32_t b = rp.pofs[di + 1];
    bool hit = false;
    for (uint32_t pi = a; pi < b; pi++) {
      uint32_t pos = rp.pos[pi];
      if (pos < rare) continue;
      uint32_t start = pos - rare;
      bool ok = true;
      for (uint16_t j = 0; j < n; j++) {
        if (j == rare) continue;
        if (!has_pos(posts[terms[q.begin + j]], doc, start + j)) {
          ok = false;
          break;
        }
      }
      if (ok) {
        hit = true;
        break;
      }
    }
    if (hit) hits.push_back(doc);
  }
  return from_ids(hits);
}

void append_u64(std::vector<char>& o, uint64_t x) {
  char tmp[32];
  int n = 0;
  do {
    tmp[n++] = static_cast<char>('0' + x % 10);
    x /= 10;
  } while (x);
  while (n--) o.push_back(tmp[n]);
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
  std::string outp = std::string(env_or("OUT", "/out")) + "/results.txt";
  Mapped docs_file(dir + "/docs.txt");
  Mapped queries_file(dir + "/queries.txt");

  using Clock = std::chrono::steady_clock;
  auto t0 = Clock::now();
  auto ms = [&] { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); };
  bool stats = std::getenv("SD_STATS") != nullptr;
  Dict dict;
  std::vector<Query> queries;
  std::vector<uint32_t> qterms;
  std::vector<uint8_t> qops;
  const char* qp = queries_file.data;
  const char* qend = queries_file.data + queries_file.len;
  while (qp < qend) {
    const char* line = qp;
    while (qp < qend && *qp != '\n') qp++;
    const char* lend = qp;
    if (qp < qend) qp++;
    Query q;
    if (lend - line >= 5 && std::memcmp(line, "BOOL ", 5) == 0) {
      q.phrase = false;
      q.begin = static_cast<uint32_t>(qterms.size());
      q.op_begin = static_cast<uint32_t>(qops.size());
      const char* p = line + 5;
      while (p < lend) {
        const char* t = p;
        while (p < lend && *p != ' ') p++;
        qterms.push_back(dict.add(t, static_cast<uint32_t>(p - t)));
        if (p >= lend) break;
        p++;
        if (p + 3 <= lend && std::memcmp(p, "AND", 3) == 0) {
          qops.push_back(kAnd);
          p += 3;
        } else if (p + 2 <= lend && std::memcmp(p, "OR", 2) == 0) {
          qops.push_back(kOr);
          p += 2;
        } else if (p + 3 <= lend && std::memcmp(p, "NOT", 3) == 0) {
          qops.push_back(kNot);
          p += 3;
        }
        if (p < lend && *p == ' ') p++;
      }
      q.n = static_cast<uint16_t>(qterms.size() - q.begin);
    } else if (lend - line >= 8 && std::memcmp(line, "PHRASE ", 7) == 0) {
      q.phrase = true;
      q.begin = static_cast<uint32_t>(qterms.size());
      const char* p = line + 7;
      if (p < lend && *p == '"') p++;
      while (p < lend && *p != '"') {
        const char* t = p;
        while (p < lend && *p != ' ' && *p != '"') p++;
        uint32_t id = dict.add(t, static_cast<uint32_t>(p - t));
        dict.need_pos[id] = 1;
        qterms.push_back(id);
        if (p < lend && *p == ' ') p++;
      }
      q.n = static_cast<uint16_t>(qterms.size() - q.begin);
    }
    queries.push_back(q);
  }
  if (stats) fprintf(stderr, "queries %.1f ms terms %u\n", ms(), dict.nterms);

  int T = worker_count();
  const char* data = docs_file.data;
  const char* end = docs_file.data + docs_file.len;
  std::vector<const char*> starts(static_cast<size_t>(T) + 1);
  starts[0] = data;
  starts[T] = end;
  for (int t = 1; t < T; t++) {
    const char* cut = data + docs_file.len * static_cast<size_t>(t) / static_cast<size_t>(T);
    if (cut > data && cut[-1] != '\n') {
      while (cut < end && cut[-1] != '\n') cut++;
    }
    starts[t] = cut;
  }
  for (int t = 1; t <= T; t++) {
    if (starts[t] < starts[t - 1]) starts[t] = starts[t - 1];
  }

  std::vector<Local> locals;
  locals.reserve(static_cast<size_t>(T));
  for (int t = 0; t < T; t++) locals.emplace_back(dict.nterms);
  std::vector<std::thread> pool;
  for (int t = 0; t < T; t++) {
    pool.emplace_back([&, t] { index_range(starts[t], starts[t + 1], dict, locals[t]); });
  }
  for (auto& th : pool) th.join();
  if (stats) fprintf(stderr, "index %.1f ms\n", ms());

  uint32_t N = 0;
  for (const Local& L : locals) N = std::max(N, L.max_id);
  uint32_t nwords = (N + 63) / 64;

  std::vector<Post> posts(dict.nterms);
  {
    std::vector<std::thread> mergers;
    int MT = std::min(T, std::max(1, static_cast<int>(dict.nterms)));
    for (int t = 0; t < MT; t++) {
      uint32_t a = dict.nterms * static_cast<uint32_t>(t) / static_cast<uint32_t>(MT);
      uint32_t b = dict.nterms * static_cast<uint32_t>(t + 1) / static_cast<uint32_t>(MT);
      mergers.emplace_back([&, a, b] {
        for (uint32_t term = a; term < b; term++) {
          Post& p = posts[term];
          bool positional = dict.need_pos[term];
          size_t nd = 0;
          size_t np = 0;
          for (const Local& L : locals) {
            nd += L.docs[term].n;
            if (positional) np += L.pos[term].n;
          }
          p.docs.reserve(nd);
          if (positional) {
            p.pos.reserve(np);
            p.pofs.reserve(nd + 1);
            p.pofs.push_back(0);
          }
          uint32_t run = 0;
          for (const Local& L : locals) {
            if (L.docs[term].n) {
              p.docs.insert(p.docs.end(), L.docs[term].p, L.docs[term].p + L.docs[term].n);
            }
            if (positional) {
              if (L.pos[term].n) {
                p.pos.insert(p.pos.end(), L.pos[term].p, L.pos[term].p + L.pos[term].n);
              }
              for (uint32_t i = 0; i < L.counts[term].n; i++) {
                run += L.counts[term].p[i];
                p.pofs.push_back(run);
              }
            }
          }
          if (p.docs.size() >= kDense) {
            p.bits.assign(nwords, 0);
            for (uint32_t d : p.docs) bit_set(p.bits, d);
          }
        }
      });
    }
    for (auto& th : mergers) th.join();
  }
  locals.clear();
  if (stats) fprintf(stderr, "merge %.1f ms N %u\n", ms(), N);

  std::vector<Ans> answers(queries.size());
  std::atomic<int> next{0};
  int QT = std::min(T, std::max(1, static_cast<int>(queries.size())));
  pool.clear();
  for (int t = 0; t < QT; t++) {
    pool.emplace_back([&] {
      Cur cur;
      std::vector<uint32_t> hits;
      for (;;) {
        int i = next.fetch_add(1, std::memory_order_relaxed);
        if (i >= static_cast<int>(queries.size())) break;
        const Query& q = queries[i];
        if (q.phrase) answers[i] = eval_phrase(q, qterms, posts, hits);
        else answers[i] = eval_bool(q, qterms, qops, posts, cur, N, nwords);
      }
    });
  }
  for (auto& th : pool) th.join();
  if (stats) fprintf(stderr, "answer %.1f ms\n", ms());

  std::vector<char> out;
  out.reserve(queries.size() * 24);
  for (const Ans& a : answers) {
    if (a.count == 0) {
      out.push_back('0');
      out.push_back(' ');
      out.push_back('0');
      out.push_back('\n');
      continue;
    }
    append_u64(out, a.count);
    out.push_back(' ');
    append_u64(out, a.sum);
    for (uint8_t i = 0; i < a.nt; i++) {
      out.push_back(' ');
      append_u64(out, a.top[i]);
    }
    out.push_back('\n');
  }

  int fd = ::open(outp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) die("open output");
  write_all(fd, out.data(), out.size());
  close(fd);
  return 0;
}
