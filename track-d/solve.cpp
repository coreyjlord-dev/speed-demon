// Forward Dijkstra on one CSR.
// The graph file is mapped once and both build passes walk it with the
// same SWAR scanner. Each edge packs a 21-bit head and an 11-bit weight;
// the few heavier weights sit in a sorted side table. A 512-node BFS
// renumber keeps a search inside a few distance pages. The queue is a
// Dial of width 8192. Build and queries use every core, up to 12.

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
  for (auto &worker : pool)
    worker.join();
}

// One mapped file. Graph build and the query list both enter through here.
const char *map_file(const char *path, size_t &bytes, int extra_flags) {
  int fd = ::open(path, O_RDONLY);
  require(fd >= 0, "open");
  struct stat info;
  require(!fstat(fd, &info), "fstat");
  bytes = (size_t)info.st_size;
  void *mapped =
      mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE | extra_flags, fd, 0);
  require(mapped != MAP_FAILED, "mmap file");
  close(fd);
  return (const char *)mapped;
}

// Sorted (edge index, weight) for the handful of weights that do not fit
// in 11 bits. Lookups happen only on those edges.
struct Heavy {
  std::vector<Edge> item;

  void absorb(EdgeGroups &groups) {
    item.clear();
    for (auto &part : groups)
      item.insert(item.end(), part.begin(), part.end());
    std::sort(item.begin(), item.end());
  }

  u32 get(u32 edge) const {
    size_t lo = 0, hi = item.size();
    while (lo < hi) {
      size_t mid = (lo + hi) >> 1;
      if (item[mid].first < edge)
        lo = mid + 1;
      else
        hi = mid;
    }
    return item[lo].second;
  }
};

struct Graph {
  u32 nodes = 0;
  u32 edge_count = 0;
  u32 *offset = nullptr;
  u32 *packed = nullptr;
  u32 *incoming = nullptr;
  u32 *rev_offset = nullptr;
  u32 *rev_packed = nullptr;
  Heavy heavy;
  Heavy rev_heavy;

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
  int spare_n = 0;
  int live_n = 0;

  void init(u32 chunks) {
    page = alloc_array<u64 *>(chunks);
    slab = alloc_array<u64>((size_t)chunks * kBlob);
    spare = alloc_array<u64 *>(chunks);
    live = alloc_array<u32>(chunks);
    for (u32 i = 0; i < chunks; ++i)
      spare[i] = slab + (size_t)i * kBlob;
    spare_n = (int)chunks;
  }

  void reset() {
    for (int i = 0; i < live_n; ++i) {
      spare[spare_n++] = page[live[i]];
      page[live[i]] = nullptr;
    }
    live_n = 0;
  }

  u64 &at(u32 vertex) {
    u32 chunk = vertex >> 9;
    u64 *&slot = page[chunk];
    if (!slot) {
      slot = spare[--spare_n];
      memset(slot, 0xFF, kBlob * sizeof(u64));
      live[live_n++] = chunk;
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

// Settled distances use the low bit. Unseen slots stay all-ones from the page
// fill.
u64 settled_on(const DistanceStore &store, u32 vertex) {
  u64 *chunk = store.page[vertex >> 9];
  if (!chunk)
    return ~0ull;
  u64 slot = chunk[vertex & 511];
  if (slot == ~0ull || !(slot & 1ull))
    return ~0ull;
  return slot >> 1;
}

bool next_labeled(Dial &queue, DistanceStore &dist, u32 &vertex, u64 &cand) {
  while (true) {
    u32 index = (u32)queue.cur & (kDial - 1);
    if (!queue.pop(index, vertex)) {
      u32 step = 1;
      while (step < kDial &&
             queue.head[(queue.cur + step) & (kDial - 1)] == ~0u)
        ++step;
      if (step < kDial) {
        queue.cur += step;
        continue;
      }
      if (!queue.take_far(cand, vertex))
        return false;
    } else {
      cand = queue.cur;
    }
    u64 slot = dist.at(vertex);
    if (slot == ~0ull || (slot >> 1) != cand || (slot & 1ull))
      continue;
    queue.cur = cand;
    return true;
  }
}

void relax_side(const u32 *offset, const u32 *packed, const Heavy &table,
                DistanceStore &mine, const DistanceStore &other, Dial &queue,
                u32 vertex, u64 cand, u64 &best) {
  mine.at(vertex) |= 1ull;
  u64 back = settled_on(other, vertex);
  if (back != ~0ull && cand + back < best)
    best = cand + back;
  u32 last = offset[vertex + 1];
  for (u32 edge = offset[vertex]; edge < last; ++edge) {
    u32 code = packed[edge];
    u32 head = code & kHeadMask;
    u64 *chunk = mine.page[head >> 9];
    if (chunk)
      __builtin_prefetch(chunk + (head & 511));
    __builtin_prefetch(packed + offset[head]);
    u32 weight = code >> kShift;
    if (weight == kHeavy)
      weight = table.get(edge);
    u64 next = cand + weight;
    if (next >= best)
      continue;
    u64 &slot = mine.at(head);
    if (slot != ~0ull && (slot >> 1) <= next)
      continue;
    slot = next << 1;
    queue.push(next, head);
    u64 meet = settled_on(other, head);
    if (meet != ~0ull && next + meet < best)
      best = next + meet;
  }
}

// Expand whichever side has settled fewer nodes. Stop when the two
// frontiers can no longer beat the best meeting, or either side runs out.
int64_t bidir(const Graph &graph, DistanceStore &forward,
              DistanceStore &backward, Dial &front, Dial &back, u32 source,
              u32 target) {
  if (source == target)
    return 0;
  if (graph.offset[source] == graph.offset[source + 1] ||
      !graph.has_incoming(target))
    return -1;
  forward.reset();
  backward.reset();
  front.clear();
  back.clear();
  forward.at(source) = 0;
  backward.at(target) = 0;
  front.push(0, source);
  back.push(0, target);
  u64 best = ~0ull;
  u64 top_f = 0;
  u64 top_b = 0;
  u64 settled_f = 0;
  u64 settled_b = 0;
  bool alive_f = true;
  bool alive_b = true;
  while (alive_f && alive_b) {
    if (best != ~0ull && top_f + top_b >= best)
      break;
    u32 vertex;
    u64 cand;
    if (settled_f <= settled_b) {
      if (!next_labeled(front, forward, vertex, cand))
        break;
      top_f = cand;
      ++settled_f;
      relax_side(graph.offset, graph.packed, graph.heavy, forward, backward,
                 front, vertex, cand, best);
    } else {
      if (!next_labeled(back, backward, vertex, cand))
        break;
      top_b = cand;
      ++settled_b;
      relax_side(graph.rev_offset, graph.rev_packed, graph.rev_heavy, backward,
                 forward, back, vertex, cand, best);
    }
  }
  return best == ~0ull ? -1 : (int64_t)best;
}

void build_reverse(Graph &graph, int workers) {
  u32 nodes = graph.nodes;
  u32 slots = graph.edge_count ? graph.edge_count : 1;
  graph.rev_offset = alloc_array<u32>(nodes + 1);
  memset(graph.rev_offset, 0, (nodes + 1ull) * 4);
  auto *degree = (std::atomic<u32> *)graph.rev_offset;
  parallel(workers, [&](int part) {
    u32 begin = (u32)((u64)nodes * part / workers);
    u32 end = (u32)((u64)nodes * (part + 1) / workers);
    for (u32 vertex = begin; vertex < end; ++vertex) {
      u32 last = graph.offset[vertex + 1];
      for (u32 edge = graph.offset[vertex]; edge < last; ++edge)
        degree[(graph.packed[edge] & kHeadMask) + 1].fetch_add(1, kRelaxed);
    }
  });
  for (u32 vertex = 1; vertex <= nodes; ++vertex)
    graph.rev_offset[vertex] += graph.rev_offset[vertex - 1];
  u32 *cursor_raw = alloc_array<u32>(nodes);
  memcpy(cursor_raw, graph.rev_offset, nodes * 4ull);
  auto *cursor = (std::atomic<u32> *)cursor_raw;
  graph.rev_packed = alloc_array<u32>(slots);
  EdgeGroups parts(workers);
  parallel(workers, [&](int part) {
    Edges &mine = parts[part];
    u32 begin = (u32)((u64)nodes * part / workers);
    u32 end = (u32)((u64)nodes * (part + 1) / workers);
    for (u32 vertex = begin; vertex < end; ++vertex) {
      u32 last = graph.offset[vertex + 1];
      for (u32 edge = graph.offset[vertex]; edge < last; ++edge) {
        u32 code = graph.packed[edge];
        u32 head = code & kHeadMask;
        u32 weight = graph.weight_of(edge, code);
        u32 index = cursor[head].fetch_add(1, kRelaxed);
        graph.rev_packed[index] = pack_edge(vertex, weight, mine, index);
      }
    }
  });
  free_array(cursor_raw, nodes, 4);
  graph.rev_heavy.absorb(parts);
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

// The only integer reader. Header, edge lines, and queries all use it.
struct Scanner {
  const char *cursor;
  const char *end;
  Scanner(const char *begin, const char *stop) : cursor(begin), end(stop) {}

  bool read(u32 &out) {
    const char *p = cursor;
    if (p >= end || (unsigned char)*p < '0')
      return false;
    u32 value = 0;
    if ((size_t)(end - p) >= 8) {
      u64 chunk;
      memcpy(&chunk, p, 8);
      int span = digit_span(chunk);
      if (span >= 8) {
        value = eight_digits(chunk);
        p += 8;
      } else {
        for (int i = 0; i < span; ++i) {
          value = value * 10u + (u32)((chunk & 255) - '0');
          chunk >>= 8;
        }
        p += span;
      }
    }
    while (p < end && (unsigned char)*p >= '0' && *p <= '9')
      value = value * 10u + (u32)(*p++ - '0');
    if (p < end && *p)
      ++p;
    cursor = p;
    out = value;
    return true;
  }
};

const char *file_bytes = nullptr;

// Both CSR passes walk these newline-aligned slices. No second skip: the
// cut already sits on the first byte of a line.
template <class Fn> void parse_range(off_t begin, off_t end, Fn fn) {
  if (begin >= end)
    return;
  Scanner scanner(file_bytes + begin, file_bytes + end);
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
    order.reserve(nodes / (u32)threads + kBlob);
    std::vector<u32> seeds;
    u32 scan = (u32)((u64)nodes * part / threads);
    u32 scan_end = (u32)((u64)nodes * (part + 1) / threads);
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
          if (!claimed[head].load(kRelaxed) && claim(head))
            order.push_back(head);
        }
      }
      for (; walk < order.size(); ++walk) {
        u32 vertex = order[walk];
        u32 last = offset[vertex + 1];
        for (u32 edge = offset[vertex]; edge < last; ++edge) {
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
}

void load(const char *path, Graph &graph, u32 *&id) {
  size_t file_bytes_n = 0;
  file_bytes = map_file(path, file_bytes_n, MAP_POPULATE);
  off_t file_end = (off_t)file_bytes_n;
  Scanner header(file_bytes, file_bytes + file_end);
  u32 declared = 0;
  header.read(graph.nodes);
  header.read(declared);
  off_t edge_at = (off_t)(header.cursor - file_bytes);

  int threads = thread_count();
  std::vector<off_t> cut(threads + 1);
  cut[0] = edge_at;
  cut[threads] = file_end;
  off_t span = file_end - edge_at;
  for (int part = 1; part < threads; ++part) {
    off_t at = edge_at + span * part / threads;
    if (at < cut[part - 1])
      at = cut[part - 1];
    while (at < file_end && file_bytes[at] != '\n')
      ++at;
    cut[part] = at < file_end ? at + 1 : file_end;
  }

  u32 nodes = graph.nodes;
  require(nodes < (1u << 21), "n");
  std::vector<u16 *> degree(threads);
  parallel(threads, [&](int part) {
    u16 *deg = alloc_array<u16>(nodes);
    memset(deg, 0, nodes * 2ull);
    degree[part] = deg;
    parse_range(cut[part], cut[part + 1],
                [&](u32 from, u32, u32) { deg[from]++; });
  });
  graph.offset = alloc_array<u32>(nodes + 1);
  graph.offset[0] = 0;
  for (u32 vertex = 0; vertex < nodes; ++vertex) {
    u32 base = 0;
    for (int part = 0; part < threads; ++part) {
      u16 have = degree[part][vertex];
      degree[part][vertex] = (u16)base;
      base += have;
    }
    graph.offset[vertex + 1] = graph.offset[vertex] + base;
  }
  graph.edge_count = graph.offset[nodes];
  u32 slots = graph.edge_count ? graph.edge_count : 1;
  graph.packed = alloc_array<u32>(slots);
  EdgeGroups heavy_parts(threads);
  parallel(threads, [&](int part) {
    u16 *base = degree[part];
    Edges &heavy = heavy_parts[part];
    parse_range(cut[part], cut[part + 1], [&](u32 from, u32 to, u32 weight) {
      u32 local = base[from]++;
      u32 index = graph.offset[from] + local;
      graph.packed[index] = pack_edge(to, weight, heavy, index);
    });
  });
  for (int part = 0; part < threads; ++part)
    free_array(degree[part], nodes, 2);
  munmap((void *)file_bytes, file_bytes_n);
  graph.heavy.absorb(heavy_parts);

  id = alloc_array<u32>(nodes);
  int workers = thread_count();
  order_blobs(nodes, graph.offset, graph.packed, id, workers);

  u32 *new_offset = alloc_array<u32>(nodes + 1);
  new_offset[0] = 0;
  parallel(workers, [&](int part) {
    u32 begin = (u32)((u64)nodes * part / workers);
    u32 end = (u32)((u64)nodes * (part + 1) / workers);
    for (u32 vertex = begin; vertex < end; ++vertex)
      new_offset[id[vertex] + 1] =
          graph.offset[vertex + 1] - graph.offset[vertex];
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
    u32 begin = (u32)((u64)nodes * part / workers);
    u32 end = (u32)((u64)nodes * (part + 1) / workers);
    for (u32 vertex = begin; vertex < end; ++vertex) {
      u32 dest = new_offset[id[vertex]];
      u32 last = graph.offset[vertex + 1];
      for (u32 edge = graph.offset[vertex]; edge < last; ++edge, ++dest) {
        u32 code = graph.packed[edge];
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
  graph.heavy.absorb(remapped);
  build_reverse(graph, workers);
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

  size_t query_bytes = 0;
  const char *queries =
      map_file((data + "/queries.txt").c_str(), query_bytes, 0);
  Scanner query_scan(queries, queries + query_bytes);
  std::vector<u32> sources, targets;
  u32 source, target;
  while (query_scan.read(source) && query_scan.read(target)) {
    sources.push_back(id[source]);
    targets.push_back(id[target]);
  }
  munmap((void *)queries, query_bytes);
  free_array(id, graph.nodes, 4);

  int query_count = (int)sources.size();
  std::vector<int> order(query_count);
  for (int i = 0; i < query_count; ++i)
    order[i] = i;
  std::sort(order.begin(), order.end(),
            [&](int a, int b) { return sources[a] < sources[b]; });

  int workers = std::clamp(thread_count(), 1, std::max(query_count, 1));
  std::vector<int64_t> answers(query_count);
  std::atomic<int> next{0};
  u32 chunks = (graph.nodes + kBlob - 1) / kBlob;
  parallel(workers, [&](int) {
    DistanceStore forward, backward;
    forward.init(chunks);
    backward.init(chunks);
    Dial front, back;
    for (;;) {
      int job = next.fetch_add(1, kRelaxed);
      if (job >= query_count)
        break;
      int query = order[job];
      answers[query] = bidir(graph, forward, backward, front, back,
                             sources[query], targets[query]);
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
