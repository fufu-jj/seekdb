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

#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace oceanbase {
namespace sql {
namespace file_sql {
// Memory bounds apply independently of input file length. Rows are never retained.
constexpr size_t MAX_RECORD = 16 * 1024 * 1024;
constexpr size_t MAX_COLUMNS = 1024;
enum class Type { NIL, BIGINT, DOUBLE, BOOLEAN, VARCHAR, DATE, DATETIME };
struct Cell {
  std::string text;
  bool null = true;
  bool quoted = false;
  bool has_hint = false;
  Type hint = Type::NIL;
};
struct Column {
  std::string name;
  Type type = Type::NIL;
  bool nullable = false;
  int scale = 0;
};
struct Options {
  std::string format, path, nullstr = "NULL";
  char delimiter = ',', quote = '"', escape = '"';
  bool header = true;
};
struct Snapshot {
  uint64_t device = 0, inode = 0;
  int64_t size = 0, mtime = 0, ctime = 0;
  bool operator==(const Snapshot &v) const;
};
struct Descriptor {
  Options options;
  Snapshot snapshot;
  std::vector<Column> columns;
  std::vector<std::string> files;
  std::string encode() const;
  bool decode(const std::string &value);
};
const char *type_name(Type t);
Type infer(const Cell &v);
Type widen(Type a, Type b);
bool integer(const std::string &s, int64_t &v);
bool real(const std::string &s, double &v);
bool date_time(const std::string &s, bool timestamp, int64_t &v);
// Root must be a nonempty secure_file_priv directory. Every path component is
// opened without following symlinks after canonicalization, including the root.
int secure_open(const std::string &path, const std::string &root, std::string &canonical,
                Snapshot &snapshot, std::string &error);
int infer_schema(Descriptor &desc, const std::string &root, std::string &error);
class CsvExporter {
 public:
  ~CsvExporter();
  int open(const Descriptor &desc, const std::string &root);
  int write(const std::vector<Cell> &row);
  int finish();
  const std::string &error() const {
    return error_;
  }

 private:
  void cleanup();
  int fail(const std::string &s);
  Descriptor desc_;
  std::string temporary_, target_, error_;
  int directory_ = -1;
  FILE *file_ = nullptr;
};
class ParquetReader;
class Scanner {
 public:
  Scanner();
  ~Scanner();
  int open(const Descriptor &desc, const std::string &root, const std::vector<int> &projection);
  int next(std::vector<Cell> &row);
  int rescan();
  int check();
  void close();
  const std::string &error() const {
    return error_;
  }

 private:
  friend int infer_schema(Descriptor &, const std::string &, std::string &);
  bool infer_mode_ = false;
  size_t records_ = 0;
  int csv(std::vector<Cell> &row);
  int json(std::vector<Cell> &row);
  int fail(const std::string &message);
  int get();
  Descriptor desc_;
  std::string root_, canonical_, error_;
  FILE *file_ = nullptr;
  int64_t line_ = 0;
  size_t file_idx_ = 0;
  std::vector<int> projection_;
  std::unique_ptr<ParquetReader> parquet_;
};
}  // namespace file_sql
}  // namespace sql
}  // namespace oceanbase
