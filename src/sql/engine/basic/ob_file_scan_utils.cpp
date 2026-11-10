/*
 * Copyright (c) 2025 OceanBase.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "sql/engine/basic/ob_file_scan_utils.h"

#include <fcntl.h>
#include <glob.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <set>

#include "rapidjson/document.h"
#include "rapidjson/stringbuffer.h"
#include "rapidjson/writer.h"
#include "share/ob_errno.h"

namespace oceanbase {
namespace sql {
namespace file_sql {
using namespace common;
namespace {
std::string lower(std::string s) {
  for (char &c : s)
    if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return s;
}
bool utf8(const std::string &s) {
  size_t i = 0;
  while (i < s.size()) {
    uint32_t c = static_cast<unsigned char>(s[i++]);
    int n = 0;
    uint32_t min = 0;
    if (c < 128) continue;
    if (c >= 0xc2 && c <= 0xdf) {
      n = 1;
      c &= 31;
      min = 128;
    } else if (c >= 0xe0 && c <= 0xef) {
      n = 2;
      c &= 15;
      min = 2048;
    } else if (c >= 0xf0 && c <= 0xf4) {
      n = 3;
      c &= 7;
      min = 65536;
    } else
      return false;
    if (i + n > s.size()) return false;
    while (n--) {
      uint32_t b = static_cast<unsigned char>(s[i++]);
      if ((b & 192) != 128) return false;
      c = (c << 6) | (b & 63);
    }
    if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
  }
  return true;
}
Snapshot snapshot_of(const struct stat &s) {
  Snapshot r;
  r.device = s.st_dev;
  r.inode = s.st_ino;
  r.size = s.st_size;
#ifdef __APPLE__
  r.mtime = s.st_mtimespec.tv_sec * 1000000000LL + s.st_mtimespec.tv_nsec;
  r.ctime = s.st_ctimespec.tv_sec * 1000000000LL + s.st_ctimespec.tv_nsec;
#else
  r.mtime = s.st_mtim.tv_sec * 1000000000LL + s.st_mtim.tv_nsec;
  r.ctime = s.st_ctim.tv_sec * 1000000000LL + s.st_ctim.tv_nsec;
#endif
  return r;
}
void put(std::string &s, const std::string &v) {
  uint32_t n = v.size();
  for (int i = 0; i < 4; i++) s.push_back((n >> (8 * i)) & 255);
  s += v;
}
bool take(const std::string &s, size_t &p, std::string &v) {
  if (p + 4 > s.size()) return false;
  uint32_t n = 0;
  for (int i = 0; i < 4; i++) n |= uint32_t(uint8_t(s[p++])) << (8 * i);
  if (n > MAX_RECORD || n > s.size() - p) return false;
  v.assign(s, p, n);
  p += n;
  return true;
}
}  // namespace
bool Snapshot::operator==(const Snapshot &s) const {
  return device == s.device && inode == s.inode && size == s.size && mtime == s.mtime &&
         ctime == s.ctime;
}
const char *type_name(Type t) {
  static const char *n[] = {"NULL", "BIGINT", "DOUBLE", "BOOLEAN", "VARCHAR", "DATE", "DATETIME"};
  return n[static_cast<int>(t)];
}
bool integer(const std::string &s, int64_t &v) {
  if (s.empty()) return false;
  size_t i = (s[0] == '-' || s[0] == '+');
  if (i == s.size()) return false;
  for (size_t j = i; j < s.size(); j++)
    if (s[j] < '0' || s[j] > '9') return false;
  errno = 0;
  char *e = nullptr;
  long long x = strtoll(s.c_str(), &e, 10);
  if (errno || *e) return false;
  v = x;
  return true;
}
bool real(const std::string &s, double &v) {
  if (s.empty() || s.find_first_of(" \t\r\n") != std::string::npos) return false;
  size_t i = (s[0] == '-' || s[0] == '+'), digits = 0;
  while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) {
    i++;
    digits++;
  }
  if (i < s.size() && s[i] == '.') {
    i++;
    while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) {
      i++;
      digits++;
    }
  }
  if (!digits) return false;
  if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
    i++;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) i++;
    size_t start = i;
    while (i < s.size() && isdigit(static_cast<unsigned char>(s[i]))) i++;
    if (i == start) return false;
  }
  if (i != s.size()) return false;
  errno = 0;
  char *e = nullptr;
  v = strtod(s.c_str(), &e);
  return !errno && !*e && std::isfinite(v);
}
bool date_time(const std::string &s, bool ts, int64_t &v) {
  if ((!ts && s.size() != 10) || (ts && (s.size() < 19 || s.size() > 26))) return false;
  auto num = [&](size_t p, size_t n) -> int {
    int x = 0;
    for (size_t i = p; i < p + n; i++) {
      if (s[i] < '0' || s[i] > '9') return -1;
      x = x * 10 + s[i] - '0';
    }
    return x;
  };
  if (s[4] != '-' || s[7] != '-') return false;
  int y = num(0, 4), m = num(5, 2), d = num(8, 2);
  bool leap = y % 4 == 0 && (y % 100 != 0 || y % 400 == 0);
  int days[] = {0, 31, 28 + leap, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (y < 1 || m < 1 || m > 12 || d < 1 || d > days[m]) return false;
  // Civil calendar to days since Unix epoch; no locale/time-zone dependence.
  int yy = y - (m <= 2), era = yy / 400, yoe = yy - era * 400, mp = m + (m > 2 ? -3 : 9);
  int64_t day =
      era * 146097 + (yoe * 365 + yoe / 4 - yoe / 100 + (153 * mp + 2) / 5 + d - 1) - 719468;
  v = day;
  if (!ts) return true;
  if ((s[10] != ' ' && s[10] != 'T') || s[13] != ':' || s[16] != ':') return false;
  int h = num(11, 2), mi = num(14, 2), sec = num(17, 2);
  if (h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 59) return false;
  int64_t us = 0;
  if (s.size() > 19) {
    if (s[19] != '.' || s.size() == 20) return false;
    int f = num(20, s.size() - 20);
    if (f < 0) return false;
    us = f;
    for (size_t n = s.size() - 20; n < 6; n++) us *= 10;
  }
  v = (day * 86400 + h * 3600 + mi * 60 + sec) * 1000000 + us;
  return true;
}
Type infer(const Cell &c) {
  if (c.null) return Type::NIL;
  if (c.has_hint) return c.hint;
  auto s = lower(c.text);
  if (s == "true" || s == "false") return Type::BOOLEAN;
  int64_t i;
  double d;
  if (integer(s, i)) return Type::BIGINT;
  bool integral = !s.empty();
  size_t start = (s[0] == '-' || s[0] == '+');
  if (start == s.size()) integral = false;
  for (size_t j = start; j < s.size(); j++)
    if (s[j] < '0' || s[j] > '9') integral = false;
  if (integral) return Type::VARCHAR;
  if (real(s, d)) return Type::DOUBLE;
  if (date_time(c.text, false, i)) return Type::DATE;
  if (date_time(c.text, true, i)) return Type::DATETIME;
  return Type::VARCHAR;
}
Type widen(Type a, Type b) {
  if (a == b || b == Type::NIL) return a;
  if (a == Type::NIL) return b;
  if ((a == Type::BIGINT && b == Type::DOUBLE) || (a == Type::DOUBLE && b == Type::BIGINT))
    return Type::DOUBLE;
  if ((a == Type::DATE && b == Type::DATETIME) || (a == Type::DATETIME && b == Type::DATE))
    return Type::DATETIME;
  return Type::VARCHAR;
}
std::string Descriptor::encode() const {
  std::string s = "SEEKDB_FILE_SQL_1";
  put(s, options.format);
  put(s, options.path);
  put(s, options.nullstr);
  s += options.delimiter;
  s += options.quote;
  s += options.escape;
  s += options.header ? '1' : '0';
  for (auto v : {snapshot.device, snapshot.inode, uint64_t(snapshot.size), uint64_t(snapshot.mtime),
                 uint64_t(snapshot.ctime)})
    put(s, std::to_string(v));
  put(s, std::to_string(columns.size()));
  for (const auto &c : columns) {
    put(s, c.name);
    s += char(c.type);
    s += c.nullable ? '1' : '0';
    s += char(c.scale);
  }
  put(s, std::to_string(files.size()));
  for (const auto &f : files) put(s, f);
  return s;
}
bool Descriptor::decode(const std::string &s) {
  if (s.compare(0, 17, "SEEKDB_FILE_SQL_1") != 0) return false;
  size_t p = 17;
  std::string x;
  if (!take(s, p, options.format) || !take(s, p, options.path) || !take(s, p, options.nullstr) ||
      p + 4 > s.size())
    return false;
  options.delimiter = s[p++];
  options.quote = s[p++];
  options.escape = s[p++];
  options.header = s[p++] == '1';
  uint64_t vals[5];
  for (auto &v : vals) {
    if (!take(s, p, x) || x.empty() || x.size() > 20) return false;
    char *e;
    errno = 0;
    v = strtoull(x.c_str(), &e, 10);
    if (errno || *e) return false;
  }
  snapshot.device = vals[0];
  snapshot.inode = vals[1];
  snapshot.size = vals[2];
  snapshot.mtime = vals[3];
  snapshot.ctime = vals[4];
  int64_t n = 0;
  if (!take(s, p, x) || !integer(x, n) || n < 0 || n > MAX_COLUMNS) return false;
  columns.clear();
  for (int i = 0; i < n; i++) {
    Column c;
    if (!take(s, p, c.name) || p + 3 > s.size()) return false;
    int t = uint8_t(s[p++]);
    if (t > 6) return false;
    c.type = Type(t);
    c.nullable = s[p++] == '1';
    c.scale = uint8_t(s[p++]);
    if (c.scale > 6) return false;
    columns.push_back(c);
  }
  if (!take(s, p, x) || !integer(x, n) || n < 0 || n > 10000) return false;
  files.clear();
  for (int i = 0; i < n; i++) {
    if (!take(s, p, x)) return false;
    files.push_back(x);
  }
  return p == s.size();
}
int secure_open(const std::string &path, const std::string &root, std::string &canonical,
                Snapshot &snap, std::string &error) {
  if (root.empty() || lower(root) == "null" || path.empty() ||
      path.find('\0') != std::string::npos || root.find('\0') != std::string::npos) {
    error = "secure_file_priv must allow a local directory";
    return -1;
  }
  char rb[4096], pb[4096];
  if (!realpath(root.c_str(), rb) || !realpath(path.c_str(), pb)) {
    error = "cannot resolve local file path";
    return -1;
  }
  std::string r = rb, p = pb;
  while (r.size() > 1 && r.back() == '/') r.pop_back();
  if (p != r && !(p.size() > r.size() && p.compare(0, r.size(), r) == 0 &&
                  (r == "/" || p[r.size()] == '/'))) {
    error = "path is outside secure_file_priv";
    return -1;
  }
  int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  size_t pos = 1;
  while (fd >= 0 && pos < p.size()) {
    size_t end = p.find('/', pos);
    bool last = end == std::string::npos;
    std::string part = p.substr(pos, last ? p.size() - pos : end - pos);
    int next = openat(fd, part.c_str(),
                      O_RDONLY | O_CLOEXEC | O_NOFOLLOW | (last ? O_NONBLOCK : O_DIRECTORY));
    ::close(fd);
    fd = next;
    if (last) break;
    pos = end + 1;
  }
  struct stat st;
  if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode)) {
    if (fd >= 0) ::close(fd);
    error = "file is not a readable regular file (or path changed)";
    return -1;
  }
  canonical = p;
  snap = snapshot_of(st);
  return fd;
}
namespace {
struct Compact {
  int64_t number = 0;
  std::string bytes;
  std::map<int, Compact> fields;
  std::vector<Compact> items;
  const Compact &at(int k) const {
    static const Compact empty;
    auto i = fields.find(k);
    return i == fields.end() ? empty : i->second;
  }
  bool has(int k) const { return fields.count(k) > 0; }
};
class CompactParser {
 public:
  const uint8_t *data;
  size_t size, pos = 0, nodes = 0;
  bool ok = true;
  CompactParser(const char *p, size_t n) : data(reinterpret_cast<const uint8_t *>(p)), size(n) {}
  uint8_t byte() {
    if (pos >= size) {
      ok = false;
      return 0;
    }
    return data[pos++];
  }
  uint64_t var() {
    uint64_t v = 0;
    for (int i = 0; i < 10; i++) {
      uint8_t b = byte();
      if (i == 9 && b > 1) {
        ok = false;
        return 0;
      }
      v |= uint64_t(b & 127) << (7 * i);
      if (!(b & 128)) return v;
    }
    ok = false;
    return 0;
  }
  int64_t zig() {
    uint64_t n = var();
    return (n >> 1) ^ -int64_t(n & 1);
  }
  Compact value(int t, int depth = 0) {
    Compact v;
    if (!ok || depth > 32 || ++nodes > 200000) {
      ok = false;
      return v;
    }
    switch (t) {
      case 1:
        v.number = 1;
        break;
      case 2:
        v.number = 0;
        break;
      case 3:
        v.number = int8_t(byte());
        break;
      case 4:
      case 5:
      case 6:
        v.number = zig();
        break;
      case 7:
        if (size - pos < 8)
          ok = false;
        else {
          v.bytes.assign(reinterpret_cast<const char *>(data + pos), 8);
          pos += 8;
        }
        break;
      case 8: {
        uint64_t n = var();
        if (n > MAX_RECORD || n > size - pos)
          ok = false;
        else {
          v.bytes.assign(reinterpret_cast<const char *>(data + pos), n);
          pos += n;
        }
        break;
      }
      case 9:
      case 10: {
        uint8_t h = byte();
        uint64_t n = h >> 4;
        int tt = h & 15;
        if (n == 15) n = var();
        if (n > 100000) {
          ok = false;
          break;
        }
        for (size_t i = 0; ok && i < n; i++) v.items.push_back(value(tt, depth + 1));
        break;
      }
      case 12: {
        int id = 0;
        while (ok) {
          uint8_t h = byte();
          if (!h) break;
          id = (h >> 4) ? id + (h >> 4) : int(zig());
          if (id <= 0 || id > INT16_MAX || v.fields.count(id)) {
            ok = false;
            break;
          }
          v.fields.emplace(id, value(h & 15, depth + 1));
        }
        break;
      }
      default:
        ok = false;
    }
    return v;
  }
};
bool read_at(int fd, int64_t offset, size_t n, std::string &s) {
  if (n > MAX_RECORD * 2 || offset < 0) return false;
  s.resize(n);
  size_t got = 0;
  while (got < n) {
    ssize_t r = pread(fd, &s[got], n - got, offset + got);
    if (r < 0 && errno == EINTR) continue;
    if (r <= 0) return false;
    got += r;
  }
  return true;
}
uint64_t little(const char *p, int n) {
  uint64_t v = 0;
  for (int i = 0; i < n; i++) v |= uint64_t(uint8_t(p[i])) << (8 * i);
  return v;
}
bool snappy_decode(const std::string &in, size_t expected, std::string &out) {
  size_t p = 0;
  uint64_t len = 0;
  int shift = 0;
  while (p < in.size() && shift < 35) {
    uint8_t b = in[p++];
    len |= uint64_t(b & 127) << shift;
    if (!(b & 128)) break;
    shift += 7;
  }
  if (len != expected || len > MAX_RECORD * 2) return false;
  out.clear();
  out.reserve(len);
  while (p < in.size() && out.size() < len) {
    uint8_t tag = in[p++];
    int type = tag & 3;
    size_t n = 0, off = 0;
    if (type == 0) {
      n = tag >> 2;
      if (n < 60)
        n++;
      else {
        int bytes = int(n) - 59;
        if (bytes > 4 || p + bytes > in.size()) return false;
        n = little(in.data() + p, bytes) + 1;
        p += bytes;
      }
      if (n > len - out.size() || n > in.size() - p) return false;
      out.append(in, p, n);
      p += n;
    } else {
      if (type == 1) {
        if (p == in.size()) return false;
        n = 4 + ((tag >> 2) & 7);
        off = ((tag & 224) << 3) | uint8_t(in[p++]);
      } else {
        int bytes = type == 2 ? 2 : 4;
        if (p + bytes > in.size()) return false;
        n = 1 + (tag >> 2);
        off = little(in.data() + p, bytes);
        p += bytes;
      }
      if (!off || off > out.size() || n > len - out.size()) return false;
      while (n--) out.push_back(out[out.size() - off]);
    }
  }
  return p == in.size() && out.size() == len;
}
bool decompress(int codec, const std::string &in, size_t n, std::string &out) {
  if (n > MAX_RECORD * 2) return false;
  if (codec == 0) {
    out = in;
    return out.size() == n;
  }
  if (codec == 1) return snappy_decode(in, n, out);
  if (codec == 2) {
    out.resize(n);
    z_stream z{};
    z.next_in = (Bytef *)in.data();
    z.avail_in = in.size();
    // A valid compressed empty page still needs an output slot so zlib can
    // consume the stream trailer. Reject any byte actually produced below.
    char empty = 0;
    z.next_out = n ? (Bytef *)&out[0] : (Bytef *)&empty;
    z.avail_out = n ? n : 1;
    if (inflateInit2(&z, 15 + 32) != Z_OK) return false;
    int rc = inflate(&z, Z_FINISH);
    bool ok = rc == Z_STREAM_END && z.total_out == n && z.avail_in == 0;
    inflateEnd(&z);
    return ok;
  }
  return false;
}
bool rle(const std::string &s, int width, size_t count, std::vector<uint32_t> &out) {
  if (width < 0 || width > 32 || count > 262144) return false;
  out.clear();
  out.reserve(count);
  size_t p = 0;
  while (p < s.size() && out.size() < count) {
    uint64_t h = 0;
    int shift = 0;
    uint8_t b;
    do {
      if (p >= s.size() || shift >= 63) return false;
      b = s[p++];
      h |= uint64_t(b & 127) << shift;
      shift += 7;
    } while (b & 128);
    if (!h) return false;
    if (!(h & 1)) {
      int bytes = (width + 7) / 8;
      if (p + bytes > s.size()) return false;
      uint64_t v = little(s.data() + p, bytes);
      p += bytes;
      if (v > UINT32_MAX) return false;
      size_t n = h >> 1;
      if (n > count - out.size()) return false;
      out.insert(out.end(), n, v);
    } else {
      // Validate the group count before multiplication; malformed 64-bit run
      // headers must not wrap around into a small apparent allocation.
      if ((h >> 1) > (count - out.size() + 7) / 8) return false;
      uint64_t n = (h >> 1) * 8, bytes = (n * width + 7) / 8;
      if (bytes > s.size() - p || n > count - out.size() + 7) return false;
      for (size_t i = 0; i < n; i++) {
        uint32_t v = 0;
        for (int bit = 0; bit < width; bit++) {
          size_t k = i * width + bit;
          v |= uint32_t((uint8_t(s[p + k / 8]) >> (k % 8)) & 1) << bit;
        }
        if (out.size() < count) out.push_back(v);
      }
      p += bytes;
    }
  }
  return out.size() == count;
}
std::string civil(int64_t day) {
  day += 719468;
  int64_t era = (day >= 0 ? day : day - 146096) / 146097;
  unsigned doe = day - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int y = yoe + era * 400;
  unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153,
           d = doy - (153 * mp + 2) / 5 + 1, m = mp + (mp < 10 ? 3 : -9);
  y += (m <= 2);
  if (y < 1 || y > 9999) return "";
  char b[32];
  snprintf(b, sizeof(b), "%04d-%02u-%02u", y, m, d);
  return b;
}
std::string timestamp_text(int64_t us) {
  int64_t day = us / 86400000000LL, rem = us % 86400000000LL;
  if (rem < 0) {
    --day;
    rem += 86400000000LL;
  }
  std::string s = civil(day);
  if (s.empty()) return s;
  char b[40];
  snprintf(b, sizeof(b), " %02lld:%02lld:%02lld.%06lld", (long long)(rem / 3600000000LL),
           (long long)(rem / 60000000 % 60), (long long)(rem / 1000000 % 60),
           (long long)(rem % 1000000));
  return s + b;
}
struct ParquetColumn {
  int physical = 0, converted = -1, length = 0;
  bool optional = false;
  int time_unit = 0;
};
struct Chunk {
  int64_t start = 0, size = 0, values = 0;
  int codec = 0;
};
struct Group {
  int64_t rows = 0;
  std::vector<Chunk> chunks;
};
struct ColumnCursor {
  int64_t offset = 0, end = 0, read = 0;
  size_t idx = 0, retained_bytes = 0;
  std::vector<Cell> page, dictionary;
};
}  // namespace
class ParquetReader {
 public:
  int fd_;
  int64_t size_;
  std::string error;
  std::vector<Column> schema;
  std::vector<ParquetColumn> meta;
  std::vector<Group> groups;
  std::vector<int> projection;
  std::vector<ColumnCursor> cursors;
  size_t group = 0;
  int64_t row = 0;
  bool active = false;
  ParquetReader(int fd, int64_t size) : fd_(fd), size_(size) {}
  int fail(const std::string &s) {
    error = "Parquet: " + s;
    return OB_ERR_UNEXPECTED;
  }
  int init() {
    std::string s, head;
    if (size_ < 12 || !read_at(fd_, 0, 4, head) || head != "PAR1" ||
        !read_at(fd_, size_ - 8, 8, s) || s.substr(4) != "PAR1")
      return fail("invalid magic/footer");
    uint64_t n = little(s.data(), 4);
    if (n > MAX_RECORD || n > uint64_t(size_ - 12) || !read_at(fd_, size_ - 8 - n, n, s))
      return fail("invalid footer size");
    CompactParser p(s.data(), s.size());
    Compact f = p.value(12);
    if (!p.ok || p.pos != s.size()) return fail("invalid compact metadata");
    auto &se = f.at(2).items;
    if (se.size() < 2 || se.size() > MAX_COLUMNS + 1 ||
        se[0].at(5).number != int64_t(se.size() - 1))
      return fail("only flat schemas are supported");
    std::set<std::string> names;
    for (size_t i = 1; i < se.size(); i++) {
      const auto &v = se[i];
      if (v.has(5) || !v.has(1) || !v.has(4) || v.at(3).number > 1)
        return fail("nested/repeated column");
      ParquetColumn m;
      m.physical = v.at(1).number;
      m.converted = v.has(6) ? v.at(6).number : -1;
      m.length = v.at(2).number;
      m.optional = v.at(3).number == 1;
      Column c;
      c.name = v.at(4).bytes;
      c.nullable = m.optional;
      if (c.name.empty() || c.name.size() > 256 || !utf8(c.name) ||
          !names.insert(lower(c.name)).second)
        return fail("invalid/duplicate column name");
      if (m.physical == 0)
        c.type = Type::BOOLEAN;
      else if (m.physical == 1 || m.physical == 2)
        c.type = Type::BIGINT;
      else if (m.physical == 4 || m.physical == 5)
        c.type = Type::DOUBLE;
      else if (m.physical == 6 || m.physical == 7)
        c.type = Type::VARCHAR;
      else if (m.physical == 3) {
        c.type = Type::DATETIME;
        m.time_unit = 3;
      } else
        return fail("unsupported primitive type");
      if (m.converted == 5) {
        if (m.physical != 1) return fail("invalid DATE physical type");
        c.type = Type::DATE;
      }
      if (m.converted == 9 || m.converted == 10) {
        if (m.physical != 2) return fail("invalid timestamp physical type");
        c.type = Type::DATETIME;
        m.time_unit = m.converted == 9 ? 1 : 2;
      }
      // LogicalType DATE (6), TIMESTAMP (8), STRING (1). Reject unsupported annotations.
      if (v.has(10)) {
        auto &lt = v.at(10);
        if (lt.has(6)) {
          c.type = Type::DATE;
          m.converted = 5;
        } else if (lt.has(8)) {
          c.type = Type::DATETIME;
          const auto &u = lt.at(8).at(2);
          m.time_unit = u.has(1) ? 1 : u.has(2) ? 2 : u.has(3) ? 4 : 0;
          if (!m.time_unit) return fail("unknown time unit");
        } else if (lt.has(10)) {
          if ((m.physical != 1 && m.physical != 2) || lt.at(10).at(2).number != 1)
            return fail("unsupported unsigned/invalid logical integer");
        } else if (!lt.has(1))
          return fail("unsupported logical type");
        if ((lt.has(6) && m.physical != 1) || (lt.has(8) && m.physical != 2) ||
            (lt.has(1) && m.physical != 6 && m.physical != 7))
          return fail("logical annotation does not match physical type");
      }
      if (m.converted >= 0 && m.converted != 0 && m.converted != 5 && m.converted != 9 &&
          m.converted != 10 && !(m.converted >= 11 && m.converted <= 18))
        return fail("unsupported converted type");
      if (m.converted >= 11 && m.converted <= 14)
        return fail("unsigned parquet integer is not supported");
      if (m.physical == 7 && (m.length <= 0 || m.length > int(MAX_RECORD)))
        return fail("invalid fixed byte length");
      if (c.type == Type::DATETIME) c.scale = m.time_unit == 1 ? 3 : 6;
      schema.push_back(c);
      meta.push_back(m);
    }
    int64_t total = 0;
    if (f.at(4).items.size() > 10000) return fail("too many row groups");
    for (const auto &rg : f.at(4).items) {
      Group g;
      g.rows = rg.at(3).number;
      if (g.rows < 0 || g.rows > INT64_MAX - total || rg.at(1).items.size() != schema.size())
        return fail("invalid row group");
      total += g.rows;
      size_t i = 0;
      for (const auto &cc : rg.at(1).items) {
        if (cc.has(1) || !cc.has(3)) return fail("external column chunk");
        auto &md = cc.at(3);
        Chunk c;
        c.codec = md.at(4).number;
        c.values = md.at(5).number;
        c.size = md.at(7).number;
        c.start = md.at(9).number;
        if (md.has(11)) c.start = std::min(c.start, md.at(11).number);
        if (md.at(1).number != meta[i].physical || md.at(3).items.size() != 1 ||
            md.at(3).items[0].bytes != schema[i].name || c.values != g.rows || c.start < 4 ||
            c.size < 0 || c.start > size_ - 8 - int64_t(n) ||
            c.size > size_ - 8 - int64_t(n) - c.start)
          return fail("invalid column chunk range");
        if (c.codec < 0 || c.codec > 2)
          return fail("only UNCOMPRESSED, SNAPPY and GZIP codecs are supported");
        g.chunks.push_back(c);
        i++;
      }
      groups.push_back(g);
    }
    if (total != f.at(3).number) return fail("row count does not match footer");
    return OB_SUCCESS;
  }
  bool plain(const std::string &s, int col, size_t count, std::vector<Cell> &out) {
    auto m = meta[col];
    size_t p = 0;
    out.clear();
    if (count > 262144) return false;
    out.reserve(count);
    for (size_t i = 0; i < count; i++) {
      Cell c;
      c.null = false;
      int n = 0;
      int64_t v = 0;
      if (m.physical == 0) {
        if (i / 8 >= s.size()) return false;
        c.text = (uint8_t(s[i / 8]) >> (i % 8)) & 1 ? "true" : "false";
      } else if (m.physical == 1 || m.physical == 2) {
        n = m.physical == 1 ? 4 : 8;
        if (p + n > s.size()) return false;
        v = m.physical == 1 ? int64_t(int32_t(little(s.data() + p, n)))
                            : int64_t(little(s.data() + p, n));
        p += n;
        if (schema[col].type == Type::DATE)
          c.text = civil(v);
        else if (schema[col].type == Type::DATETIME) {
          if (m.time_unit == 1) {
            if (v > INT64_MAX / 1000 || v < INT64_MIN / 1000) return false;
            v *= 1000;
          } else if (m.time_unit == 4)
            v /= 1000;
          c.text = timestamp_text(v);
        } else
          c.text = std::to_string(v);
        if (c.text.empty()) return false;
      } else if (m.physical == 3) {
        if (p + 12 > s.size()) return false;
        uint64_t nanos = little(s.data() + p, 8);
        int64_t days = int64_t(little(s.data() + p + 8, 4)) - 2440588;
        p += 12;
        if (nanos >= 86400000000000ULL || days > 3000000 || days < -3000000) return false;
        c.text = timestamp_text(days * 86400000000LL + nanos / 1000);
        if (c.text.empty()) return false;
      } else if (m.physical == 4 || m.physical == 5) {
        n = m.physical == 4 ? 4 : 8;
        if (p + n > s.size()) return false;
        double d;
        if (n == 4) {
          uint32_t bits = little(s.data() + p, 4);
          float f;
          memcpy(&f, &bits, 4);
          d = f;
        } else {
          uint64_t bits = little(s.data() + p, 8);
          memcpy(&d, &bits, 8);
        }
        p += n;
        if (!std::isfinite(d)) return false;
        char b[64];
        snprintf(b, sizeof(b), "%.17g", d);
        c.text = b;
      } else {
        if (m.physical == 6) {
          if (p + 4 > s.size()) return false;
          uint64_t len = little(s.data() + p, 4);
          p += 4;
          if (len > MAX_RECORD || len > s.size() - p) return false;
          n = len;
        } else
          n = m.length;
        if (n < 0 || size_t(n) > s.size() - p) return false;
        c.text = s.substr(p, n);
        p += n;
        if (!utf8(c.text)) return false;
      }
      out.push_back(std::move(c));
    }
    return m.physical == 0 ? (count + 7) / 8 == s.size() : p == s.size();
  }
  int page(int col, ColumnCursor &cur) {
    const auto &chunk = groups[group].chunks[col];
    while (cur.offset < cur.end) {
      std::string buf;
      if (!read_at(fd_, cur.offset, std::min<int64_t>(65536, cur.end - cur.offset), buf))
        return fail("truncated page header");
      CompactParser p(buf.data(), buf.size());
      Compact h = p.value(12);
      if (!p.ok || !h.has(1) || !h.has(2) || !h.has(3)) return fail("invalid page header");
      int64_t raw = h.at(2).number, compressed = h.at(3).number, kind = h.at(1).number;
      if (raw < 0 || compressed < 0 || raw > int64_t(MAX_RECORD * 2) ||
          compressed > int64_t(MAX_RECORD * 2) ||
          compressed > cur.end - cur.offset - int64_t(p.pos))
        return fail("invalid page length");
      cur.offset += p.pos;
      if (!read_at(fd_, cur.offset, compressed, buf)) return fail("truncated page");
      cur.offset += compressed;
      std::string data;
      if (kind == 2) {
        if (!decompress(chunk.codec, buf, raw, data) || !h.has(7) || h.at(7).at(2).number != 0)
          return fail("invalid dictionary page");
        int64_t n = h.at(7).at(1).number;
        if (n < 0 || !plain(data, col, n, cur.dictionary)) return fail("invalid dictionary values");
        continue;
      }
      if (kind != 0 && kind != 3) continue;
      auto &dh = h.at(kind == 0 ? 5 : 8);
      int64_t count = dh.at(1).number, encoding = dh.at(kind == 0 ? 2 : 4).number;
      if (count <= 0 || count > 262144 || count > chunk.values - cur.read)
        return fail("invalid data page value count");
      std::vector<uint32_t> levels;
      std::string values;
      size_t off = 0;
      if (kind == 0) {
        if (!decompress(chunk.codec, buf, raw, data)) return fail("decompression failed");
        if (meta[col].optional) {
          if (dh.at(3).number != 3 || data.size() < 4) return fail("unsupported definition levels");
          size_t len = little(data.data(), 4);
          off = 4;
          if (len > data.size() - off || !rle(data.substr(off, len), 1, count, levels))
            return fail("invalid definition levels");
          off += len;
        }
        values = data.substr(off);
      } else {
        int64_t defs = dh.at(5).number, reps = dh.at(6).number;
        if (reps != 0 || defs < 0 || defs > compressed || defs > raw)
          return fail("invalid v2 levels");
        if (meta[col].optional && !rle(buf.substr(0, defs), 1, count, levels))
          return fail("invalid v2 definition levels");
        bool compressed_data = !dh.has(7) || dh.at(7).number != 0;
        if (!decompress(compressed_data ? chunk.codec : 0, buf.substr(defs), raw - defs, values))
          return fail("v2 decompression failed");
      }
      if (!meta[col].optional) levels.assign(count, 1);
      size_t present = 0;
      for (auto v : levels) {
        if (v > 1) return fail("definition level exceeds schema");
        present += v;
      }
      std::vector<Cell> decoded;
      if (encoding == 0) {
        if (!plain(values, col, present, decoded)) return fail("invalid PLAIN values");
      } else if (encoding == 3 && meta[col].physical == 0) {
        if (values.size() < 4 || little(values.data(), 4) != values.size() - 4)
          return fail("invalid boolean RLE length");
        std::vector<uint32_t> bits;
        if (!rle(values.substr(4), 1, present, bits)) return fail("invalid boolean RLE");
        for (auto bit : bits) {
          if (bit > 1) return fail("invalid boolean value");
          Cell c;
          c.null = false;
          c.text = bit ? "true" : "false";
          decoded.push_back(c);
        }
      } else if (encoding == 2 || encoding == 8) {
        if (values.empty()) return fail("missing dictionary indices");
        std::vector<uint32_t> ids;
        if (!rle(values.substr(1), uint8_t(values[0]), present, ids))
          return fail("invalid dictionary indices");
        size_t decoded_bytes = 0;
        for (auto id : ids) {
          if (id >= cur.dictionary.size()) return fail("dictionary index out of range");
          size_t bytes = sizeof(Cell) + cur.dictionary[id].text.size();
          if (bytes > 128 * 1024 * 1024 - decoded_bytes)
            return fail("decoded dictionary page memory limit exceeded");
          decoded_bytes += bytes;
          decoded.push_back(cur.dictionary[id]);
        }
      } else
        return fail("unsupported data encoding");
      cur.page.clear();
      cur.idx = 0;
      size_t idx = 0;
      for (auto v : levels) {
        if (v)
          cur.page.push_back(std::move(decoded[idx++]));
        else
          cur.page.emplace_back();
      }
      cur.retained_bytes = 0;
      for (const auto &x : cur.page) cur.retained_bytes += sizeof(Cell) + x.text.size();
      for (const auto &x : cur.dictionary) cur.retained_bytes += sizeof(Cell) + x.text.size();
      if (cur.retained_bytes > 128 * 1024 * 1024) return fail("column page memory limit exceeded");
      cur.read += count;
      return OB_SUCCESS;
    }
    return fail("column ended before row group");
  }
  int next(std::vector<Cell> &out) {
    while (group < groups.size() && row == groups[group].rows) {
      group++;
      row = 0;
      active = false;
    }
    if (group == groups.size()) return OB_ITER_END;
    if (!active) {
      cursors.clear();
      cursors.resize(projection.size());
      for (size_t i = 0; i < projection.size(); i++) {
        auto &ch = groups[group].chunks[projection[i]];
        cursors[i].offset = ch.start;
        cursors[i].end = ch.start + ch.size;
      }
      active = true;
    }
    out.assign(schema.size(), Cell());
    size_t used = 0;
    for (size_t i = 0; i < projection.size(); i++) {
      int col = projection[i];
      auto &c = cursors[i];
      if (c.idx == c.page.size()) {
        int rc = page(col, c);
        if (rc) return rc;
      }
      used += c.retained_bytes;
      if (used > 128 * 1024 * 1024) return fail("projected page memory limit exceeded");
      out[col] = c.page[c.idx++];
    }
    ++row;
    return OB_SUCCESS;
  }
};
Scanner::Scanner() = default;
Scanner::~Scanner() { close(); }
void Scanner::close() {
  parquet_.reset();
  if (file_) {
    fclose(file_);
    file_ = nullptr;
  }
}
int Scanner::fail(const std::string &s) {
  error_ = desc_.options.path + ":" + std::to_string(line_) + ": " + s;
  return OB_ERR_UNEXPECTED;
}
int Scanner::get() { return fgetc(file_); }
int Scanner::open(const Descriptor &d, const std::string &root,
                  const std::vector<int> &projection) {
  close();
  desc_ = d;
  root_ = root;
  projection_ = projection;
  line_ = 0;
  file_idx_ = 0;
  error_.clear();
  if (d.options.format == "glob") return OB_SUCCESS;
  Snapshot snap;
  int fd = secure_open(d.options.path, root, canonical_, snap, error_);
  if (fd < 0) return OB_ERR_NO_PRIVILEGE;
  if (!(snap == d.snapshot)) {
    ::close(fd);
    return fail("file changed since schema inference");
  }
  file_ = fdopen(fd, "rb");
  if (!file_) {
    ::close(fd);
    return fail("cannot open file stream");
  }
  if (d.options.format == "parquet") {
    parquet_.reset(new ParquetReader(fd, snap.size));
    int rc = parquet_->init();
    if (rc) {
      error_ = parquet_->error;
      return rc;
    }
    parquet_->projection = projection;
  } else if (d.options.format == "csv" && d.options.header && !d.columns.empty()) {
    std::vector<Cell> header;
    int rc = csv(header);
    if (rc) return rc;
  }
  return OB_SUCCESS;
}
int Scanner::rescan() {
  Descriptor d = desc_;
  std::string root = root_;
  auto p = projection_;
  return open(d, root, p);
}
int Scanner::check() {
  if (!file_) return OB_SUCCESS;
  struct stat st;
  Snapshot snap;
  std::string path, error;
  if (fstat(fileno(file_), &st) || !(snapshot_of(st) == desc_.snapshot))
    return fail("file changed during scan");
  int fd = secure_open(desc_.options.path, root_, path, snap, error);
  if (fd < 0) return fail("file path changed during scan");
  ::close(fd);
  if (!(snap == desc_.snapshot) || path != canonical_) return fail("file was replaced during scan");
  return OB_SUCCESS;
}
int Scanner::csv(std::vector<Cell> &row) {
  row.clear();
  Cell c;
  c.null = false;
  bool inside = false, after = false, started = false;
  size_t bytes = 0;
  const auto &o = desc_.options;
  auto finish = [&]() -> bool {
    if (!utf8(c.text)) return false;
    c.null = !c.quoted && (c.text.empty() || c.text == o.nullstr);
    row.push_back(std::move(c));
    c = Cell();
    c.null = false;
    return row.size() <= MAX_COLUMNS;
  };
  // Consume a UTF-8 BOM once, without seeking backwards over a buffered record.
  if (ftell(file_) == 0) {
    int a = get();
    if (a == 0xef) {
      int b = get(), e = get();
      if (b != 0xbb || e != 0xbf) return fail("invalid UTF-8 BOM");
    } else if (a != EOF)
      ungetc(a, file_);
  }
  while (true) {
    int ch = get();
    if (ch == EOF) {
      if (ferror(file_)) return fail("read error");
      if (inside) return fail("unterminated quoted field");
      if (!started && row.empty() && c.text.empty()) return OB_ITER_END;
      if (!finish()) return fail("too many columns or invalid UTF-8");
      ++line_;
      return OB_SUCCESS;
    }
    started = true;
    if (++bytes > MAX_RECORD) return fail("record exceeds 16 MiB limit");
    if (inside) {
      if (ch == o.quote) {
        int next = get();
        if (next == o.quote && o.escape == o.quote)
          c.text += char(ch);
        else {
          inside = false;
          after = true;
          if (next != EOF) ungetc(next, file_);
        }
      } else if (ch == o.escape && o.escape != o.quote) {
        int next = get();
        if (next == EOF) return fail("incomplete quoted escape");
        if (next != o.escape && next != o.quote) return fail("invalid quoted escape");
        c.text += char(next);
      } else
        c.text += char(ch);
      continue;
    }
    if (ch == o.delimiter) {
      if (!finish()) return fail("too many columns or invalid UTF-8");
      after = false;
      continue;
    }
    if (ch == '\n' || ch == '\r') {
      if (ch == '\r') {
        int n = get();
        if (n != '\n' && n != EOF) ungetc(n, file_);
      }
      if (!finish()) return fail("too many columns or invalid UTF-8");
      ++line_;
      return OB_SUCCESS;
    }
    if (after) return fail("characters after closing quote");
    if (ch == o.quote) {
      if (!c.text.empty()) return fail("quote inside unquoted field");
      inside = true;
      c.quoted = true;
    } else
      c.text += char(ch);
  }
}
int Scanner::json(std::vector<Cell> &row) {
  std::string s;
  int ch;
read_line:
  s.clear();
  while ((ch = get()) != EOF) {
    if (ch == '\n') break;
    if (s.size() >= MAX_RECORD) return fail("JSONL record exceeds 16 MiB limit");
    s += char(ch);
  }
  if (ch == EOF && ferror(file_)) return fail("read error");
  if (s.empty() && ch == EOF) return OB_ITER_END;
  ++line_;
  if (line_ == 1 && s.compare(0, 3, "\xef\xbb\xbf") == 0) s.erase(0, 3);
  if (s.find_first_not_of(" \t\r") == std::string::npos) goto read_line;
  bool quoted = false, escaped = false;
  int depth = 0;
  for (char c : s) {
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        quoted = false;
    } else if (c == '"')
      quoted = true;
    else if (c == '{' || c == '[') {
      if (++depth > 1 || c == '[') return fail("nested JSON values are not supported");
    } else if (c == '}' || c == ']')
      --depth;
  }
  rapidjson::Document d;
  d.Parse<rapidjson::kParseIterativeFlag | rapidjson::kParseValidateEncodingFlag |
          rapidjson::kParseFullPrecisionFlag>(s.data(), s.size());
  if (d.HasParseError() || !d.IsObject())
    return fail("expected a flat JSON object (byte " + std::to_string(d.GetErrorOffset()) + ")");
  std::map<std::string, Cell> fields;
  for (auto it = d.MemberBegin(); it != d.MemberEnd(); ++it) {
    std::string name(it->name.GetString(), it->name.GetStringLength());
    if (name.empty() || name.size() > 256 || fields.count(lower(name)))
      return fail("empty or duplicate JSON field");
    Cell c;
    auto &v = it->value;
    c.null = v.IsNull();
    if (v.IsObject() || v.IsArray()) return fail("nested JSON values are not supported");
    c.has_hint = true;
    if (v.IsString()) {
      c.text.assign(v.GetString(), v.GetStringLength());
      int64_t date = 0;
      c.hint = date_time(c.text, false, date)  ? Type::DATE
               : date_time(c.text, true, date) ? Type::DATETIME
                                               : Type::VARCHAR;
    } else if (v.IsBool()) {
      c.text = v.GetBool() ? "true" : "false";
      c.hint = Type::BOOLEAN;
    } else if (v.IsInt64()) {
      c.text = std::to_string(v.GetInt64());
      c.hint = Type::BIGINT;
    } else if (v.IsUint64()) {
      c.text = std::to_string(v.GetUint64());
      c.hint = Type::VARCHAR;
    } else if (v.IsDouble()) {
      c.hint = Type::DOUBLE;
      if (!std::isfinite(v.GetDouble())) return fail("non-finite JSON number");
      char b[64];
      snprintf(b, sizeof(b), "%.17g", v.GetDouble());
      c.text = b;
    }
    fields.emplace(lower(name), c);
    if (infer_mode_) {
      auto pos = std::find_if(desc_.columns.begin(), desc_.columns.end(),
                              [&](const Column &a) { return lower(a.name) == lower(name); });
      if (pos == desc_.columns.end()) {
        if (desc_.columns.size() >= MAX_COLUMNS) return fail("too many columns");
        Column col;
        col.name = name;
        col.nullable = records_ > 0;
        desc_.columns.push_back(col);
      }
    }
  }
  row.assign(desc_.columns.size(), Cell());
  for (size_t i = 0; i < desc_.columns.size(); i++) {
    auto it = fields.find(lower(desc_.columns[i].name));
    if (it != fields.end()) row[i] = it->second;
    if (infer_mode_) {
      auto &col = desc_.columns[i];
      col.type = widen(col.type, infer(row[i]));
      col.nullable |= row[i].null;
      if (!row[i].null && infer(row[i]) == Type::DATETIME)
        col.scale = std::max(col.scale, int(row[i].text.size() > 19 ? row[i].text.size() - 20 : 0));
    }
  }
  ++records_;
  return OB_SUCCESS;
}
int Scanner::next(std::vector<Cell> &row) {
  if (desc_.options.format == "glob") {
    if (file_idx_ == desc_.files.size()) return OB_ITER_END;
    Snapshot s;
    std::string path;
    int fd = secure_open(desc_.files[file_idx_], root_, path, s, error_);
    if (fd < 0) return OB_ERR_NO_PRIVILEGE;
    ::close(fd);
    Cell c;
    c.null = false;
    c.text = desc_.files[file_idx_++];
    row = {c};
    return OB_SUCCESS;
  }
  int rc = check();
  if (rc) return rc;
  if (parquet_) {
    rc = parquet_->next(row);
    if (rc && rc != OB_ITER_END) error_ = parquet_->error;
  } else if (desc_.options.format == "csv") {
    rc = csv(row);
    if (!rc && row.size() != desc_.columns.size())
      return fail("CSV field count differs from header/schema");
  } else
    rc = json(row);
  if (rc == OB_SUCCESS || rc == OB_ITER_END) {
    int c = check();
    if (c) return c;
  }
  return rc;
}
int infer_schema(Descriptor &desc, const std::string &root, std::string &error) {
  if (desc.options.format == "glob") {
    auto p = desc.options.path.find_first_of("*?[");
    std::string prefix = desc.options.path.substr(0, p);
    size_t slash = prefix.rfind('/');
    std::string dir = slash == std::string::npos ? "." : prefix.substr(0, slash + 1);
    char canonical[4096], rb[4096];
    if (root.empty() || lower(root) == "null" || !realpath(root.c_str(), rb) ||
        !realpath(dir.c_str(), canonical)) {
      error = "glob directory is not allowed";
      return OB_ERR_NO_PRIVILEGE;
    }
    std::string r = rb, c = canonical;
    if (c != r && !(c.size() > r.size() && c.compare(0, r.size(), r) == 0 &&
                    (r == "/" || c[r.size()] == '/'))) {
      error = "glob is outside secure_file_priv";
      return OB_ERR_NO_PRIVILEGE;
    }
    glob_t g{};
    int status = ::glob(desc.options.path.c_str(), GLOB_NOSORT, nullptr, &g);
    if (status != 0 && status != GLOB_NOMATCH) {
      globfree(&g);
      error = "glob discovery failed";
      return OB_ERR_UNEXPECTED;
    }
    if (g.gl_pathc > 10000) {
      globfree(&g);
      error = "glob file limit exceeded";
      return OB_SIZE_OVERFLOW;
    }
    for (size_t i = 0; i < g.gl_pathc; i++) {
      struct stat st;
      if (lstat(g.gl_pathv[i], &st)) {
        globfree(&g);
        return OB_ERR_UNEXPECTED;
      }
      if (S_ISDIR(st.st_mode)) continue;
      Snapshot snap;
      std::string path;
      int fd = secure_open(g.gl_pathv[i], root, path, snap, error);
      if (fd < 0) {
        globfree(&g);
        return OB_ERR_NO_PRIVILEGE;
      }
      ::close(fd);
      desc.files.push_back(path);
    }
    globfree(&g);
    std::sort(desc.files.begin(), desc.files.end());
    desc.columns = {{"file", Type::VARCHAR, false}};
    return OB_SUCCESS;
  }
  std::string path;
  int fd = secure_open(desc.options.path, root, path, desc.snapshot, error);
  if (fd < 0) return OB_ERR_NO_PRIVILEGE;
  if (desc.options.format == "parquet") {
    ParquetReader reader(fd, desc.snapshot.size);
    int rc = reader.init();
    if (rc)
      error = reader.error;
    else
      desc.columns = reader.schema;
    struct stat st;
    if (!rc && (fstat(fd, &st) || !(snapshot_of(st) == desc.snapshot))) {
      error = "file changed during schema inference";
      rc = OB_ERR_UNEXPECTED;
    }
    ::close(fd);
    return rc;
  }
  ::close(fd);
  Scanner scan;
  int rc = scan.open(desc, root, {});
  if (rc) {
    error = scan.error();
    return rc;
  }
  scan.infer_mode_ = true;
  scan.records_ = 0;
  if (desc.options.format == "csv") {
    std::vector<Cell> row;
    rc = scan.csv(row);
    if (rc == OB_ITER_END) {
      error = "CSV file has no columns";
      return OB_INVALID_ARGUMENT;
    }
    if (rc) {
      error = scan.error();
      return rc;
    }
    std::set<std::string> names;
    for (size_t i = 0; i < row.size(); i++) {
      std::string name = desc.options.header ? row[i].text : "column" + std::to_string(i);
      if (name.empty()) name = "column" + std::to_string(i);
      std::string original = name;
      int n = 2;
      while (names.count(lower(name))) name = original + "__" + std::to_string(n++);
      if (name.size() > 256) {
        error = "column name exceeds 256 bytes";
        return OB_SIZE_OVERFLOW;
      }
      names.insert(lower(name));
      desc.columns.push_back({name, Type::NIL, false});
    }
    auto absorb = [&]() -> bool {
      if (row.size() != desc.columns.size()) return false;
      for (size_t i = 0; i < row.size(); i++) {
        auto &c = desc.columns[i];
        c.type = widen(c.type, infer(row[i]));
        c.nullable |= row[i].null;
        if (!row[i].null && infer(row[i]) == Type::DATETIME)
          c.scale = std::max(c.scale, int(row[i].text.size() > 19 ? row[i].text.size() - 20 : 0));
      }
      return true;
    };
    if (!desc.options.header && !absorb()) return OB_ERR_UNEXPECTED;
    while ((rc = scan.csv(row)) == OB_SUCCESS) {
      if (!absorb()) {
        error = desc.options.path + ":" + std::to_string(scan.line_) + ": CSV field count mismatch";
        return OB_ERR_UNEXPECTED;
      }
    }
  } else {
    std::vector<Cell> row;
    while ((rc = scan.json(row)) == OB_SUCCESS) {
    }
    desc.columns = scan.desc_.columns;
  }
  if (rc != OB_ITER_END) {
    error = scan.error();
    return rc;
  }
  rc = scan.check();
  if (rc) error = scan.error();
  if (desc.columns.empty()) {
    error = "file has no inferable columns";
    return OB_INVALID_ARGUMENT;
  }
  return rc;
}
}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
namespace oceanbase {
namespace sql {
namespace file_sql {
CsvExporter::~CsvExporter() { cleanup(); }
void CsvExporter::cleanup() {
  if (file_) {
    fclose(file_);
    file_ = nullptr;
  }
  if (directory_ >= 0) {
    if (!temporary_.empty()) unlinkat(directory_, temporary_.c_str(), 0);
    ::close(directory_);
    directory_ = -1;
  }
}
int CsvExporter::fail(const std::string &s) {
  error_ = "COPY: " + s;
  cleanup();
  return OB_ERR_UNEXPECTED;
}
int CsvExporter::open(const Descriptor &desc, const std::string &root) {
  cleanup();
  desc_ = desc;
  auto &o = desc_.options;
  if (o.path.empty() || o.path.find('\0') != std::string::npos || root.empty() ||
      lower(root) == "null")
    return fail("invalid path or secure_file_priv");
  if (!o.delimiter || !o.quote || !o.escape || o.delimiter == o.quote || o.delimiter == '\n' ||
      o.delimiter == '\r' || o.quote == '\n' || o.quote == '\r' || o.escape == '\n' ||
      o.escape == '\r' ||
      o.nullstr.find_first_of(std::string() + o.delimiter + o.quote + "\r\n") != std::string::npos)
    return fail("invalid CSV options");
  size_t slash = o.path.rfind('/');
  std::string parent = slash == std::string::npos ? "." : o.path.substr(0, slash + 1);
  target_ = slash == std::string::npos ? o.path : o.path.substr(slash + 1);
  if (target_.empty() || target_ == "." || target_ == "..") return fail("invalid output file name");
  char p[4096], r[4096];
  if (!realpath(parent.c_str(), p) || !realpath(root.c_str(), r))
    return fail("cannot resolve output directory");
  std::string dir = p, allowed = r;
  if (dir != allowed &&
      !(dir.size() > allowed.size() && dir.compare(0, allowed.size(), allowed) == 0 &&
        (allowed == "/" || dir[allowed.size()] == '/')))
    return fail("output is outside secure_file_priv");
  directory_ = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  size_t pos = 1;
  while (directory_ >= 0 && pos < dir.size()) {
    size_t end = dir.find('/', pos);
    std::string part = dir.substr(pos, end == std::string::npos ? dir.size() - pos : end - pos);
    int fd = openat(directory_, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    ::close(directory_);
    directory_ = fd;
    if (end == std::string::npos) break;
    pos = end + 1;
  }
  if (directory_ < 0) return fail("output directory changed");
  struct stat st;
  if (fstatat(directory_, target_.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT)
    return fail("target already exists or cannot be checked");
  for (int i = 0; i < 100; i++) {
    temporary_ = ".seekdb-copy-" + std::to_string(getpid()) + "-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(i);
    int fd = openat(directory_, temporary_.c_str(),
                    O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd >= 0) {
      file_ = fdopen(fd, "wb");
      if (!file_) {
        ::close(fd);
        return fail("cannot create output stream");
      }
      break;
    }
    if (errno != EEXIST) return fail("cannot create temporary output");
  }
  if (!file_) return fail("temporary file collision limit");
  if (o.header) {
    std::vector<Cell> row;
    for (auto &c : desc.columns) {
      Cell v;
      v.null = false;
      v.text = c.name;
      row.push_back(v);
    }
    return write(row);
  }
  return OB_SUCCESS;
}
int CsvExporter::write(const std::vector<Cell> &row) {
  if (!file_ || row.size() != desc_.columns.size()) return fail("invalid export row");
  std::string line;
  auto &o = desc_.options;
  for (size_t i = 0; i < row.size(); i++) {
    if (i) line += o.delimiter;
    if (row[i].null)
      line += o.nullstr;
    else {
      line += o.quote;
      for (char c : row[i].text) {
        if (c == o.quote || c == o.escape) line += o.escape;
        line += c;
      }
      line += o.quote;
    }
    if (line.size() > MAX_RECORD) return fail("export record exceeds 16 MiB limit");
  }
  line += '\n';
  if (fwrite(line.data(), 1, line.size(), file_) != line.size()) return fail("output write failed");
  return OB_SUCCESS;
}
int CsvExporter::finish() {
  if (!file_) return fail("output stream is closed");
  if (fflush(file_) || fsync(fileno(file_))) return fail("cannot flush output");
  if (fclose(file_)) {
    file_ = nullptr;
    return fail("cannot close output");
  }
  file_ = nullptr;
  // linkat is atomic and never replaces an existing target, including a symlink.
  if (linkat(directory_, temporary_.c_str(), directory_, target_.c_str(), 0))
    return fail("target exists or atomic publish failed");
  unlinkat(directory_, temporary_.c_str(), 0);
  temporary_.clear();
  ::close(directory_);
  directory_ = -1;
  return OB_SUCCESS;
}
}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
