// Forward Dijkstra on one CSR.
// The graph file is mapped and parsed with SWAR. Each edge packs a 21-bit
// head with an 11-bit weight; heavier weights live in a side table. A
// 512-node BFS renumber keeps a search inside a few distance pages. The
// queue is a Dial of width 8192. Build and queries use every core, up to 12.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <malloc.h>
#include <string>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using Edge = std::pair<u32, u32>;
using Edges = std::vector<Edge>;
using EdgeGroups = std::vector<Edges>;

constexpr auto kRelaxed = std::memory_order_relaxed;
constexpr int kBlob = 512;
constexpr u32 kDial = 8192;
constexpr u32 kHeadMask = (1u << 21) - 1;
constexpr u32 kShift = 21;
constexpr u32 kHeavy = 2047;

[[noreturn]] void die(const char *message) {
  fprintf(stderr, "track-d: %s\n", message);
  exit(1);
}

void require(bool ok, const char *message) {
  if (!ok)
    die(message);
}

int thread_count() {
  if (const char *env = std::getenv("SD_THREADS")) {
    int n = std::atoi(env);
    if (n >= 1 && n <= 64)
      return n;
  }
  unsigned cores = std::thread::hardware_concurrency();
  return cores ? (int)std::min(cores, 12u) : 1;
}

template <class T> T *alloc_array(size_t count) {
  if (!count)
    count = 1;
  void *memory = mmap(nullptr, count * sizeof(T), PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  require(memory != MAP_FAILED, "mmap");
  return (T *)memory;
}

void free_array(void *memory, size_t count, size_t elem) {
  if (memory)
    munmap(memory, (count ? count : 1) * elem);
}

template <class Fn> void parallel(int threads, Fn fn) {
  std::vector<std::thread> pool;
  pool.reserve(threads);
  for (int t = 0; t < threads; ++t)
    pool.emplace_back(fn, t);
  for (auto &thread : pool)
    thread.join();
}

// Read a decimal and step past the following separator when one is present.
u32 read_u32(char *&cursor) {
  u32 value = 0;
  while (*cursor >= '0')
    value = value * 10u + (u32)(*cursor++ - '0');
  if (*cursor)
    ++cursor;
  return value;
}

// Open-addressed map from edge index to a weight that does not fit in 11 bits.
struct HeavyWeights {
  u32 *keys = nullptr;
  u32 *values = nullptr;
  u32 mask = 0;
  u32 capacity = 0;

  void clear() {
    if (!keys)
      return;
    free_array(keys, capacity, 4);
    free_array(values, capacity, 4);
    keys = values = nullptr;
    capacity = 0;
  }

  u32 probe(u32 edge) const {
    u32 slot = (edge * 0x9E3779B1u) & mask;
    while (keys[slot] != ~0u && keys[slot] != edge)
      slot = (slot + 1) & mask;
    return slot;
  }

  void build(const Edges &items) {
    clear();
    u32 cap = 8;
    while (cap < items.size() * 2u + 8u)
      cap <<= 1;
    capacity = cap;
    mask = cap - 1;
    keys = alloc_array<u32>(cap);
    values = alloc_array<u32>(cap);
    memset(keys, 0xFF, cap * 4ull);
    for (auto [edge, weight] : items) {
      u32 slot = probe(edge);
      keys[slot] = edge;
      values[slot] = weight;
    }
  }

  u32 get(u32 edge) const { return values[probe(edge)]; }
};

void build_heavy(HeavyWeights &table, const EdgeGroups &groups) {
  Edges heavy;
  for (auto &part : groups)
    heavy.insert(heavy.end(), part.begin(), part.end());
  table.build(heavy);
}

struct Graph {
  u32 nodes = 0;
  u32 edge_count = 0;
  u32 *offset = nullptr;
  u32 *packed = nullptr;
  u32 *incoming = nullptr;
  HeavyWeights heavy;

  bool has_incoming(u32 vertex) const {
    return (incoming[vertex >> 5] >> (vertex & 31)) & 1u;
  }

  u32 weight_of(u32 index, u32 code) const {
    u32 weight = code >> kShift;
    return weight == kHeavy ? heavy.get(index) : weight;
  }
};

u32 pack_edge(u32 head, u32 weight, Edges &heavy, u32 index) {
  if (weight >= kHeavy) {
    heavy.emplace_back(index, weight);
    weight = kHeavy;
  }
  return head | (weight << kShift);
}

// Recycled distance pages. A chunk is faulted on first touch and returned
// to the spare list when the query ends.
struct DistanceStore {
  u64 **page = nullptr;
  u64 *slab = nullptr;
  u64 **spare = nullptr;
  u32 *live = nullptr;
  int spare_count = 0;
  int live_count = 0;

  void init(u32 chunks) {
    page = alloc_array<u64 *>(chunks);
    slab = alloc_array<u64>((size_t)chunks * kBlob);
    spare = alloc_array<u64 *>(chunks);
    live = alloc_array<u32>(chunks);
    for (u32 i = 0; i < chunks; ++i)
      spare[i] = slab + (size_t)i * kBlob;
    spare_count = (int)chunks;
  }

  void reset() {
    for (int i = 0; i < live_count; ++i) {
      spare[spare_count++] = page[live[i]];
      page[live[i]] = nullptr;
    }
    live_count = 0;
  }

  u64 &at(u32 vertex) {
    u32 chunk = vertex >> 9;
    u64 *&slot = page[chunk];
    if (!slot) {
      require(spare_count, "pages");
      slot = spare[--spare_count];
      memset(slot, 0xFF, kBlob * sizeof(u64));
      live[live_count++] = chunk;
    }
    return slot[vertex & 511];
  }
};

// A distance that jumps at least one full Dial turn.
struct Pending {
  u64 distance;
  u32 vertex;
};

// One pool of bucket links. A vertex may sit in several buckets; stale
// entries are skipped when they are popped.
struct Dial {
  struct Node {
    u32 vertex;
    u32 next;
  };
  std::vector<Node> pool;
  u32 head[kDial];
  std::vector<int> touched;
  std::vector<Pending> far;
  u64 cur = 0;

  Dial() { std::fill(head, head + kDial, ~0u); }

  void clear() {
    for (int index : touched)
      head[index] = ~0u;
    touched.clear();
    pool.clear();
    far.clear();
    cur = 0;
  }

  void push(u64 distance, u32 vertex) {
    if (distance - cur >= kDial) {
      far.push_back({distance, vertex});
      return;
    }
    u32 index = (u32)distance & (kDial - 1);
    if (head[index] == ~0u)
      touched.push_back((int)index);
    pool.push_back({vertex, head[index]});
    head[index] = (u32)pool.size() - 1;
  }

  bool pop(u32 index, u32 &vertex) {
    u32 slot = head[index];
    if (slot == ~0u)
      return false;
    vertex = pool[slot].vertex;
    head[index] = pool[slot].next;
    return true;
  }

  bool take_far(u64 &distance, u32 &vertex) {
    if (far.empty())
      return false;
    size_t best = 0;
    for (size_t i = 1; i < far.size(); ++i)
      if (far[i].distance < far[best].distance)
        best = i;
    distance = far[best].distance;
    vertex = far[best].vertex;
    far[best] = far.back();
    far.pop_back();
    return true;
  }
};

int64_t dijkstra(const Graph &graph, DistanceStore &dist, Dial &queue,
                 u32 source, u32 target) {
  if (source == target)
    return 0;
  const u32 *offset = graph.offset;
  const u32 *packed = graph.packed;
  if (offset[source] == offset[source + 1] || !graph.has_incoming(target))
    return -1;
  dist.reset();
  queue.clear();
  dist.at(source) = 0;
  queue.push(0, source);
  auto relax = [&](u32 u) {
    for (u32 edge = offset[u]; edge < offset[u + 1]; ++edge) {
      u32 code = packed[edge];
      u32 head = code & kHeadMask;
      u64 next = queue.cur + graph.weight_of(edge, code);
      u64 &best = dist.at(head);
      if (next < best) {
        best = next;
        queue.push(next, head);
      }
    }
  };
  // 1 = target settled, 0 = keep searching. A stale entry leaves cur alone.
  auto settle = [&](u32 u, u64 distance) {
    if (dist.at(u) != distance)
      return 0;
    queue.cur = distance;
    if (u == target)
      return 1;
    relax(u);
    return 0;
  };
  while (true) {
    u32 index = (u32)queue.cur & (kDial - 1);
    u32 vertex;
    if (!queue.pop(index, vertex)) {
      u32 step = 1;
      while (step < kDial &&
             queue.head[(queue.cur + step) & (kDial - 1)] == ~0u)
        ++step;
      if (step < kDial) {
        queue.cur += step;
        continue;
      }
      u64 far;
      if (!queue.take_far(far, vertex))
        return -1;
      if (settle(vertex, far))
        return (int64_t)queue.cur;
      continue;
    }
    if (settle(vertex, queue.cur))
      return (int64_t)queue.cur;
  }
}

// Eight decimal digits, little-endian, via SWAR.
u32 eight_digits(u64 chunk) {
  chunk = (chunk & 0x0F0F0F0F0F0F0F0Full) * 2561 >> 8;
  chunk = (chunk & 0x00FF00FF00FF00FFull) * 6553601 >> 16;
  chunk = (chunk & 0x0000FFFF0000FFFFull) * 42949672960001ull >> 32;
  return (u32)chunk;
}

// How many leading bytes of the chunk are digits. Eight means the whole word.
int digit_span(u64 chunk) {
  u64 high = (chunk & 0xF0F0F0F0F0F0F0F0ull) ^ 0x3030303030303030ull;
  u64 low = ((chunk & 0x0F0F0F0F0F0F0F0Full) + 0x0606060606060606ull) &
            0xF0F0F0F0F0F0F0F0ull;
  u64 bad = high | low;
  if (!bad)
    return 8;
  return __builtin_ctzll(bad) >> 3;
}

int graph_fd = -1;
off_t edge_offset = 0;
off_t file_end = 0;
const char *file_bytes = nullptr;

struct Scanner {
  const char *cursor;
  const char *end;
  Scanner(off_t begin, off_t stop)
      : cursor(file_bytes + begin), end(file_bytes + stop) {}

  bool read(u32 &out) {
    const char *&p = cursor;
    if (p >= end || (unsigned char)*p < '0')
      return false;
    size_t left = (size_t)(end - p);
    if (left >= 8) {
      u64 chunk;
      memcpy(&chunk, p, 8);
      int span = digit_span(chunk);
      if (span >= 8) {
        out = eight_digits(chunk);
        p += 8;
        while (p < end && (unsigned char)*p >= '0' && *p <= '9')
          out = out * 10u + *p++ - '0';
      } else {
        out = 0;
        for (int i = 0; i < span; ++i) {
          out = out * 10u + (u32)((chunk & 255) - '0');
          chunk >>= 8;
        }
        p += span;
      }
    } else {
      out = 0;
      while (p < end && (unsigned char)*p >= '0' && *p <= '9')
        out = out * 10u + *p++ - '0';
    }
    if (p < end && *p)
      ++p;
    return true;
  }
};

template <class Fn> void parse_range(off_t begin, off_t end, Fn fn) {
  if (begin >= end)
    return;
  Scanner scanner(begin, end);
  if (begin != edge_offset) {
    while (scanner.cursor < scanner.end && *scanner.cursor != '\n')
      ++scanner.cursor;
    if (scanner.cursor < scanner.end)
      ++scanner.cursor;
  }
  u32 from, to, weight;
  while (scanner.read(from) && scanner.read(to) && scanner.read(weight))
    if (from != to)
      fn(from, to, weight);
}

// Claim nodes into per-thread BFS blobs of kBlob so later ids are local.
void order_blobs(u32 nodes, u32 *offset, u32 *packed, u32 *id, int threads) {
  auto *claimed = alloc_array<std::atomic<u8>>(nodes);
  memset(claimed, 0, nodes);
  std::vector<std::vector<u32>> chains(threads);
  parallel(threads, [&](int part) {
    auto &order = chains[part];
    order.reserve((nodes / (u32)threads) + kBlob);
    std::vector<u32> seeds;
    u32 scan = u32((u64)nodes * part / threads);
    u32 scan_end = u32((u64)nodes * (part + 1) / threads);
    auto claim = [&](u32 vertex) {
      u8 expected = 0;
      return claimed[vertex].compare_exchange_strong(expected, 1, kRelaxed);
    };
    for (;;) {
      u32 seed = nodes;
      while (!seeds.empty() && seed == nodes) {
        u32 candidate = seeds.back();
        seeds.pop_back();
        if (claim(candidate))
          seed = candidate;
      }
      while (seed == nodes && scan < scan_end) {
        u32 candidate = scan++;
        if (offset[candidate] != offset[candidate + 1] && claim(candidate))
          seed = candidate;
      }
      if (seed == nodes)
        break;
      size_t base = order.size();
      size_t walk = base;
      size_t limit = base + (size_t)kBlob;
      order.push_back(seed);
      for (; walk < order.size() && order.size() < limit; ++walk) {
        u32 vertex = order[walk];
        u32 last = offset[vertex + 1];
        for (u32 edge = offset[vertex]; edge < last && order.size() < limit;
             ++edge) {
          u32 head = packed[edge] & kHeadMask;
          if (claim(head))
            order.push_back(head);
        }
      }
      for (; walk < order.size(); ++walk) {
        u32 vertex = order[walk];
        for (u32 edge = offset[vertex]; edge < offset[vertex + 1]; ++edge) {
          u32 head = packed[edge] & kHeadMask;
          if (!claimed[head].load(kRelaxed))
            seeds.push_back(head);
        }
      }
    }
  });
  u32 next = 0;
  for (auto &order : chains)
    for (u32 vertex : order)
      id[vertex] = next++;
  for (u32 vertex = 0; vertex < nodes; ++vertex)
    if (!claimed[vertex].load(kRelaxed))
      id[vertex] = next++;
  free_array(claimed, nodes, 1);
  require(next == nodes, "order");
}

void load(const char *path, Graph &graph, u32 *&id) {
  graph_fd = ::open(path, O_RDONLY);
  require(graph_fd >= 0, "open graph");
  struct stat info;
  require(!fstat(graph_fd, &info), "fstat");
  void *mapped = mmap(nullptr, (size_t)info.st_size, PROT_READ,
                      MAP_PRIVATE | MAP_POPULATE, graph_fd, 0);
  require(mapped != MAP_FAILED, "mmap file");
  file_bytes = (const char *)mapped;
  char header[64]{};
  require(::pread(graph_fd, header, 63, 0) >= 2, "header");
  char *header_cursor = header;
  graph.nodes = read_u32(header_cursor);
  read_u32(header_cursor);
  edge_offset = 0;
  while (header[edge_offset] && header[edge_offset] != '\n')
    ++edge_offset;
  ++edge_offset;
  file_end = info.st_size;

  int threads = thread_count();
  if (file_end - edge_offset < (1 << 20))
    threads = 1;
  std::vector<off_t> cut(threads + 1);
  cut[0] = edge_offset;
  cut[threads] = file_end;
  off_t span = file_end - edge_offset;
  for (int part = 1; part < threads; ++part) {
    off_t at = edge_offset + span * part / threads;
    char window[96];
    ssize_t got = ::pread(graph_fd, window, 95, at);
    int skip = 0;
    while (skip < got && window[skip] != '\n')
      ++skip;
    cut[part] = at + (skip < got ? skip + 1 : 0);
    if (cut[part] < cut[part - 1])
      cut[part] = cut[part - 1];
  }

  u32 nodes = graph.nodes;
  require(nodes < (1u << 21), "n");
  // Pass 1. One u16 degree array per thread, never an edge buffer.
  std::vector<u16 *> degrees(threads);
  parallel(threads, [&](int part) {
    u16 *degree = alloc_array<u16>(nodes);
    memset(degree, 0, nodes * 2ull);
    degrees[part] = degree;
    parse_range(cut[part], cut[part + 1], [&](u32 from, u32, u32) {
      if (from >= nodes || degree[from] == 65535)
        die("degree");
      degree[from]++;
    });
  });
  graph.offset = alloc_array<u32>(nodes + 1);
  graph.offset[0] = 0;
  for (u32 vertex = 0; vertex < nodes; ++vertex) {
    u32 sum = 0;
    for (int part = 0; part < threads; ++part)
      sum += degrees[part][vertex];
    graph.offset[vertex + 1] = graph.offset[vertex] + sum;
  }
  graph.edge_count = graph.offset[nodes];
  // Turn per-thread degrees into bases within each vertex's edge block.
  for (u32 vertex = 0; vertex < nodes; ++vertex) {
    u32 base = 0;
    for (int part = 0; part < threads; ++part) {
      u16 have = degrees[part][vertex];
      if (have == 65535 || base > 65535)
        die("degree");
      degrees[part][vertex] = (u16)base;
      base += have;
    }
  }
  u32 slots = graph.edge_count ? graph.edge_count : 1;
  graph.packed = alloc_array<u32>(slots);
  EdgeGroups heavy_parts(threads);
  // Pass 2. Same file, private bases, no edge buffer and no atomic cursor.
  parallel(threads, [&](int part) {
    u16 *base = degrees[part];
    Edges &heavy = heavy_parts[part];
    parse_range(cut[part], cut[part + 1], [&](u32 from, u32 to, u32 weight) {
      if (from >= nodes || to >= nodes)
        return;
      u32 local = base[from];
      if (local == 65535)
        die("degree");
      base[from] = (u16)(local + 1);
      u32 index = graph.offset[from] + local;
      graph.packed[index] = pack_edge(to, weight, heavy, index);
    });
  });
  for (int part = 0; part < threads; ++part)
    free_array(degrees[part], nodes, 2);
  munmap((void *)file_bytes, (size_t)file_end);
  close(graph_fd);
  build_heavy(graph.heavy, heavy_parts);
  id = alloc_array<u32>(nodes);
  order_blobs(nodes, graph.offset, graph.packed, id, thread_count());

  u32 *new_offset = alloc_array<u32>(nodes + 1);
  memset(new_offset, 0, (nodes + 1ull) * 4);
  int workers = thread_count();
  parallel(workers, [&](int part) {
    u32 begin = u32((u64)nodes * part / workers);
    u32 end = u32((u64)nodes * (part + 1) / workers);
    u32 *off = graph.offset;
    for (u32 vertex = begin; vertex < end; ++vertex)
      new_offset[id[vertex] + 1] = off[vertex + 1] - off[vertex];
  });
  for (u32 i = 1; i <= nodes; ++i)
    new_offset[i] += new_offset[i - 1];

  u32 words = (nodes + 31) / 32 + 1;
  graph.incoming = alloc_array<u32>(words);
  memset(graph.incoming, 0, words * 4ull);
  auto *incoming_bits = (std::atomic<u32> *)graph.incoming;
  u32 *new_packed = alloc_array<u32>(slots);
  EdgeGroups remapped(workers);
  parallel(workers, [&](int part) {
    Edges &mine = remapped[part];
    u32 *off = graph.offset;
    u32 *pack = graph.packed;
    u32 begin = u32((u64)nodes * part / workers);
    u32 end = u32((u64)nodes * (part + 1) / workers);
    for (u32 vertex = begin; vertex < end; ++vertex) {
      u32 dest = new_offset[id[vertex]];
      for (u32 edge = off[vertex]; edge < off[vertex + 1]; ++edge, ++dest) {
        u32 code = pack[edge];
        u32 head = id[code & kHeadMask];
        u32 weight = code >> kShift;
        if (weight == kHeavy)
          weight = graph.heavy.get(edge);
        new_packed[dest] = pack_edge(head, weight, mine, dest);
        incoming_bits[head >> 5].fetch_or(1u << (head & 31), kRelaxed);
      }
    }
  });
  free_array(graph.packed, slots, 4);
  free_array(graph.offset, nodes + 1, 4);
  graph.offset = new_offset;
  graph.packed = new_packed;
  build_heavy(graph.heavy, remapped);
}

void append_i64(char *&out, int64_t value) {
  if (value < 0) {
    *out++ = '-';
    value = -value;
  }
  char digits[24];
  int count = 0;
  u64 magnitude = (u64)value;
  do
    digits[count++] = (char)('0' + magnitude % 10);
  while (magnitude /= 10);
  while (count--)
    *out++ = digits[count];
  *out++ = '\n';
}

const char *env_or(const char *key, const char *fallback) {
  const char *value = std::getenv(key);
  return value && *value ? value : fallback;
}

} // namespace

int main() {
  prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0);
  mallopt(M_MMAP_THRESHOLD, 64 * 1024);
  mallopt(M_TRIM_THRESHOLD, 64 * 1024);
  std::string data = env_or("DATA", "/data");
  std::string out_path = std::string(env_or("OUT", "/out")) + "/distances.txt";
  Graph graph;
  u32 *id = nullptr;
  load((data + "/graph.txt").c_str(), graph, id);

  int query_fd = ::open((data + "/queries.txt").c_str(), O_RDONLY);
  require(query_fd >= 0, "open queries");
  struct stat query_info;
  require(!fstat(query_fd, &query_info), "fstat q");
  size_t size = (size_t)query_info.st_size;
  char *query_bytes = alloc_array<char>(size + 1);
  size_t got = 0;
  while (got < size) {
    ssize_t n = ::read(query_fd, query_bytes + got, size - got);
    require(n > 0, "read q");
    got += (size_t)n;
  }
  close(query_fd);
  query_bytes[got] = 0;
  std::vector<u32> sources, targets;
  for (char *cursor = query_bytes; *cursor;) {
    u32 source = read_u32(cursor);
    u32 target = read_u32(cursor);
    sources.push_back(id[source]);
    targets.push_back(id[target]);
  }
  free_array(query_bytes, size + 1, 1);
  free_array(id, graph.nodes, 4);

  int query_count = (int)sources.size();
  std::vector<int> order(query_count);
  for (int i = 0; i < query_count; ++i)
    order[i] = i;
  auto by_source = [&](int a, int b) { return sources[a] < sources[b]; };
  std::sort(order.begin(), order.end(), by_source);

  int workers = std::clamp(thread_count(), 1, std::max(query_count, 1));
  std::vector<int64_t> answers(query_count);
  std::atomic<int> next{0};
  u32 chunks = (graph.nodes + kBlob - 1) / kBlob;
  parallel(workers, [&](int) {
    DistanceStore distances;
    distances.init(chunks);
    Dial queue;
    for (;;) {
      int job = next.fetch_add(1, kRelaxed);
      if (job >= query_count)
        break;
      int query = order[job];
      answers[query] =
          dijkstra(graph, distances, queue, sources[query], targets[query]);
    }
  });

  char *out = alloc_array<char>((size_t)query_count * 24 + 8);
  char *cursor = out;
  for (int64_t distance : answers)
    append_i64(cursor, distance);
  int out_fd = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  require(out_fd >= 0, "open out");
  size_t left = (size_t)(cursor - out);
  const char *write_at = out;
  while (left) {
    ssize_t n = ::write(out_fd, write_at, left);
    require(n >= 0, "write");
    write_at += n;
    left -= (size_t)n;
  }
  close(out_fd);
  return 0;
}
