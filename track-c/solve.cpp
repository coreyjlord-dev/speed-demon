// Track C (Join): quote-aware CSV parse, array join, name bitsets.
//
// records.csv is hostile (quotes, escaped quotes, embedded LF, mixed
// CRLF/LF, BOMs on some record boundaries). The parser follows the
// spec byte for byte. regions.csv is a plain table; region ids are
// dense enough for a direct array. Unknown ids land in _UNMATCHED.
//
// Distinct names are exact unescaped bytes. Each worker interns the
// names it sees and sets a bit per country. After the scan the bits
// are unioned through a global intern table, so the same name in two
// workers counts once. Amounts stay in integer cents.
//
// Workers split the file on record starts. A record start is a digit
// that begins an id. Section 2.2 guarantees an embedded LF in a note
// is followed by a letter or a quote, never by a digit, and a BOM
// appears only at a record boundary. The chunk cutter uses that so
// workers can start at a real record; the parser itself is quote-aware.

#include <algorithm>
#include <array>
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

[[noreturn]] void die(const char* msg) {
  fprintf(stderr, "track-c: %s\n", msg);
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

bool is_bom(const char* p, const char* end) {
  return p + 3 <= end && static_cast<unsigned char>(p[0]) == 0xEF &&
         static_cast<unsigned char>(p[1]) == 0xBB &&
         static_cast<unsigned char>(p[2]) == 0xBF;
}

struct Agg {
  uint64_t count = 0;
  uint64_t id_sum = 0;
  uint64_t name_bytes = 0;
  uint64_t note_bytes = 0;
  int64_t amount_sum = 0;
  int64_t amin = INT64_MAX;
  int64_t amax = INT64_MIN;
};

struct Slot {
  uint64_t hash = 0;
  uint32_t off = 0;
  uint32_t len = 0;
  uint32_t id = kEmpty;
};

struct Intern {
  std::string bytes;
  std::vector<Slot> slots;
  std::vector<uint32_t> span_off;
  std::vector<uint32_t> span_len;
  uint32_t mask = 0;
  uint32_t used = 0;

  Intern() { rehash(1024); }

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

  static uint64_t mix(const char* s, uint32_t n) {
    uint64_t h = 0x9E3779B185EBCA87ull ^ (static_cast<uint64_t>(n) << 1);
    for (uint32_t i = 0; i < n; i++) {
      h ^= static_cast<unsigned char>(s[i]);
      h *= 0x100000001B3ull;
    }
    return h ^ (h >> 33);
  }

  uint32_t intern(const char* s, uint32_t n) {
    uint64_t h = mix(s, n);
    if ((used + 1) * 10 >= (mask + 1) * 7) rehash((mask + 1) * 2);
    uint32_t i = static_cast<uint32_t>(h) & mask;
    for (;;) {
      Slot& sl = slots[i];
      if (sl.id == kEmpty) {
        sl.hash = h;
        sl.off = static_cast<uint32_t>(bytes.size());
        sl.len = n;
        sl.id = static_cast<uint32_t>(span_off.size());
        bytes.append(s, n);
        span_off.push_back(sl.off);
        span_len.push_back(n);
        used++;
        return sl.id;
      }
      if (sl.hash == h && sl.len == n &&
          std::memcmp(bytes.data() + sl.off, s, n) == 0) {
        return sl.id;
      }
      i = (i + 1) & mask;
    }
  }
};

struct Local {
  std::vector<Agg> ag;
  Intern names;
  std::vector<std::vector<uint64_t>> bits;

  explicit Local(size_t countries) : ag(countries), bits(countries) {}

  void add_name(uint32_t country, uint32_t name_id) {
    auto& b = bits[country];
    size_t word = name_id >> 6;
    if (b.size() <= word) {
      size_t ns = b.empty() ? 16 : b.size() * 2;
      if (ns <= word) ns = word + 1;
      b.resize(ns, 0);
    }
    b[word] |= 1ull << (name_id & 63);
  }
};

struct World {
  std::vector<int32_t> region_country;  // -1 unknown
  std::vector<std::array<char, 3>> codes;
  int code_slot[26 * 26 * 26];
  uint32_t unmatched = 0;

  World() { std::fill(std::begin(code_slot), std::end(code_slot), -1); }

  uint32_t country_of(char a, char b, char c) {
    int s = (a - 'A') * 676 + (b - 'A') * 26 + (c - 'A');
    if (code_slot[s] >= 0) return static_cast<uint32_t>(code_slot[s]);
    uint32_t id = static_cast<uint32_t>(codes.size());
    code_slot[s] = static_cast<int>(id);
    codes.push_back({a, b, c});
    return id;
  }
};

void load_regions(const Mapped& file, World& w) {
  const char* p = file.data;
  const char* end = file.data + file.len;
  // header
  while (p < end && *p != '\n') p++;
  if (p < end) p++;
  uint32_t max_id = 0;
  struct Row {
    uint32_t id;
    char a, b, c;
  };
  std::vector<Row> rows;
  rows.reserve(file.len / 10);
  while (p < end) {
    uint32_t id = 0;
    while (p < end && *p >= '0' && *p <= '9') {
      id = id * 10u + static_cast<uint32_t>(*p - '0');
      p++;
    }
    if (p >= end || *p != ',') break;
    p++;
    char a = *p++;
    char b = *p++;
    char c = *p++;
    if (p < end && *p == '\n') p++;
    rows.push_back({id, a, b, c});
    if (id > max_id) max_id = id;
  }
  w.region_country.assign(static_cast<size_t>(max_id) + 1, -1);
  for (const Row& r : rows) {
    uint32_t cid = w.country_of(r.a, r.b, r.c);
    w.region_country[r.id] = static_cast<int32_t>(cid);
  }
  w.unmatched = static_cast<uint32_t>(w.codes.size());
}

uint32_t country_index(const World& w, uint32_t region) {
  if (region >= w.region_country.size()) return w.unmatched;
  int32_t c = w.region_country[region];
  if (c < 0) return w.unmatched;
  return static_cast<uint32_t>(c);
}

// Parse one record. p is on the first digit of id. Returns the next byte.
const char* parse_one(const char* p, const char* end, const World& w, Local& loc,
                      std::vector<char>& name) {
  uint64_t id = 0;
  while (p < end && *p >= '0' && *p <= '9') {
    id = id * 10u + static_cast<uint32_t>(*p - '0');
    p++;
  }
  p++;  // comma

  // Unescaped bytes are usually a raw slice of the file. Copy only when
  // a quoted field contains "" escapes.
  const char* name_ptr = nullptr;
  uint32_t name_len = 0;
  if (*p == '"') {
    p++;
    const char* s = p;
    bool escaped = false;
    for (;;) {
      if (*p == '"') {
        if (p[1] == '"') {
          escaped = true;
          p += 2;
        } else {
          break;
        }
      } else {
        p++;
      }
    }
    uint32_t raw_n = static_cast<uint32_t>(p - s);
    p++;  // closing quote
    if (!escaped) {
      name_ptr = s;
      name_len = raw_n;
    } else {
      name.clear();
      for (uint32_t i = 0; i < raw_n; i++) {
        if (s[i] == '"' && i + 1 < raw_n && s[i + 1] == '"') {
          name.push_back('"');
          i++;
        } else {
          name.push_back(s[i]);
        }
      }
      name_ptr = name.data();
      name_len = static_cast<uint32_t>(name.size());
    }
  } else {
    name_ptr = p;
    while (*p != ',') p++;
    name_len = static_cast<uint32_t>(p - name_ptr);
  }
  p++;  // comma

  uint32_t note_len = 0;
  if (*p == '"') {
    p++;
    for (;;) {
      if (*p == '"') {
        if (p[1] == '"') {
          note_len++;
          p += 2;
        } else {
          p++;
          break;
        }
      } else {
        note_len++;
        p++;
      }
    }
  } else {
    while (*p != ',') {
      note_len++;
      p++;
    }
  }
  p++;  // comma

  uint32_t region = 0;
  while (*p >= '0' && *p <= '9') {
    region = region * 10u + static_cast<uint32_t>(*p - '0');
    p++;
  }
  p++;  // comma

  int sign = 1;
  if (*p == '-') {
    sign = -1;
    p++;
  }
  int64_t whole = 0;
  while (*p != '.') {
    whole = whole * 10 + (*p - '0');
    p++;
  }
  p++;
  int frac = (*p - '0') * 10 + (*(p + 1) - '0');
  p += 2;
  int64_t cents = sign * (whole * 100 + frac);
  if (p < end && *p == '\r') p++;
  if (p < end && *p == '\n') p++;

  uint32_t cid = country_index(w, region);
  Agg& a = loc.ag[cid];
  a.count++;
  a.id_sum += id;
  a.amount_sum += cents;
  if (cents < a.amin) a.amin = cents;
  if (cents > a.amax) a.amax = cents;
  a.name_bytes += name_len;
  a.note_bytes += note_len;
  uint32_t nid = loc.names.intern(name_ptr, name_len);
  loc.add_name(cid, nid);
  return p;
}

const char* find_record_start(const char* pos, const char* end) {
  for (const char* q = pos; q < end; q++) {
    if (*q != '\n') continue;
    const char* r = q + 1;
    if (is_bom(r, end)) r += 3;
    if (r < end && *r >= '1' && *r <= '9') return r;
  }
  return end;
}

void parse_range(const char* p, const char* limit, const char* end, const World& w, Local& loc) {
  std::vector<char> name;
  name.reserve(128);
  while (p < end) {
    const char* q = p;
    if (is_bom(q, end)) q += 3;
    if (q >= limit || q >= end) break;
    p = parse_one(q, end, w, loc, name);
  }
}

void set_bit(std::vector<uint64_t>& b, uint32_t id) {
  size_t word = id >> 6;
  if (b.size() <= word) {
    size_t ns = b.empty() ? 16 : b.size() * 2;
    if (ns <= word) ns = word + 1;
    b.resize(ns, 0);
  }
  b[word] |= 1ull << (id & 63);
}

uint32_t popcount_bits(const std::vector<uint64_t>& b) {
  uint32_t n = 0;
  for (uint64_t w : b) n += static_cast<uint32_t>(__builtin_popcountll(w));
  return n;
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

void append_money(std::vector<char>& o, int64_t cents) {
  if (cents < 0) {
    o.push_back('-');
    cents = -cents;
  }
  append_u64(o, static_cast<uint64_t>(cents / 100));
  o.push_back('.');
  int f = static_cast<int>(cents % 100);
  o.push_back(static_cast<char>('0' + f / 10));
  o.push_back(static_cast<char>('0' + f % 10));
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
  std::string outp = std::string(env_or("OUT", "/out")) + "/aggregates.txt";
  Mapped regions(dir + "/regions.csv");
  Mapped records(dir + "/records.csv");

  World world;
  load_regions(regions, world);
  size_t countries = static_cast<size_t>(world.unmatched) + 1;

  const char* data = records.data;
  const char* end = records.data + records.len;
  const char* p = data;
  if (is_bom(p, end)) p += 3;
  while (p < end && *p != '\n') p++;
  if (p < end) p++;
  const char* first = p;

  int T = worker_count();
  std::vector<const char*> starts(static_cast<size_t>(T) + 1);
  starts[0] = first;
  starts[T] = end;
  for (int t = 1; t < T; t++) {
    const char* cut = data + records.len * static_cast<size_t>(t) / static_cast<size_t>(T);
    if (cut < first) cut = first;
    starts[t] = find_record_start(cut, end);
  }
  for (int t = 1; t <= T; t++) {
    if (starts[t] < starts[t - 1]) starts[t] = starts[t - 1];
  }

  std::vector<Local> locals;
  locals.reserve(static_cast<size_t>(T));
  for (int t = 0; t < T; t++) locals.emplace_back(countries);

  std::vector<std::thread> pool;
  for (int t = 0; t < T; t++) {
    pool.emplace_back([&, t] {
      parse_range(starts[t], starts[t + 1], end, world, locals[t]);
    });
  }
  for (auto& th : pool) th.join();

  std::vector<Agg> ag(countries);
  std::vector<std::vector<uint64_t>> gbits(countries);
  Intern global_names;
  for (Local& loc : locals) {
    for (size_t c = 0; c < countries; c++) {
      Agg& dst = ag[c];
      const Agg& src = loc.ag[c];
      if (src.count == 0) continue;
      dst.count += src.count;
      dst.id_sum += src.id_sum;
      dst.name_bytes += src.name_bytes;
      dst.note_bytes += src.note_bytes;
      dst.amount_sum += src.amount_sum;
      if (src.amin < dst.amin) dst.amin = src.amin;
      if (src.amax > dst.amax) dst.amax = src.amax;
      const auto& bits = loc.bits[c];
      for (size_t w = 0; w < bits.size(); w++) {
        uint64_t word = bits[w];
        while (word) {
          uint32_t b = static_cast<uint32_t>(__builtin_ctzll(word));
          uint32_t lid = static_cast<uint32_t>(w * 64 + b);
          uint32_t off = loc.names.span_off[lid];
          uint32_t len = loc.names.span_len[lid];
          uint32_t gid = global_names.intern(loc.names.bytes.data() + off, len);
          set_bit(gbits[c], gid);
          word &= word - 1;
        }
      }
    }
  }

  std::vector<uint32_t> order;
  order.reserve(world.codes.size());
  for (uint32_t c = 0; c < world.unmatched; c++) {
    if (ag[c].count) order.push_back(c);
  }
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
    return std::memcmp(world.codes[a].data(), world.codes[b].data(), 3) < 0;
  });
  if (ag[world.unmatched].count) order.push_back(world.unmatched);

  std::vector<char> out;
  out.reserve(order.size() * 80 + 64);
  for (uint32_t c : order) {
    const Agg& a = ag[c];
    if (c == world.unmatched) {
      out.insert(out.end(), "_UNMATCHED", "_UNMATCHED" + 10);
    } else {
      out.insert(out.end(), world.codes[c].data(), world.codes[c].data() + 3);
    }
    out.push_back(' ');
    append_u64(out, a.count);
    out.push_back(' ');
    append_u64(out, a.id_sum);
    out.push_back(' ');
    append_money(out, a.amount_sum);
    out.push_back(' ');
    append_money(out, a.amin);
    out.push_back(' ');
    append_money(out, a.amax);
    out.push_back(' ');
    append_u64(out, popcount_bits(gbits[c]));
    out.push_back(' ');
    append_u64(out, a.name_bytes);
    out.push_back(' ');
    append_u64(out, a.note_bytes);
    out.push_back('\n');
  }

  int fd = ::open(outp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) die("open output");
  write_all(fd, out.data(), out.size());
  close(fd);
  return 0;
}
