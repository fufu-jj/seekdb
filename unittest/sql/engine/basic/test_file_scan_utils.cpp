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

// Standalone scanner regression: compile with ob_file_scan_utils.cpp, include
// src/, src/oblib/, dependency headers, and link zlib. No database mock is used.
#include <unistd.h>

#include <cassert>
#include <fstream>
#include <iostream>

#include "share/ob_errno.h"
#include "sql/engine/basic/ob_file_scan_utils.h"
using namespace oceanbase::sql::file_sql;
using namespace oceanbase::common;
int main(int argc, char **argv) {
  assert(argc == 2);
  std::string root = argv[1];
  Cell v;
  v.null = false;
  for (auto s : {"2024-02-29", "0001-01-01", "9999-12-31"}) {
    v.text = s;
    assert(infer(v) == Type::DATE);
  }
  for (auto s :
       {"2023-02-29", "2026-13-01", "2026-01-00", "9223372036854775808", "-9223372036854775809"}) {
    v.text = s;
    assert(infer(v) == Type::VARCHAR);
  }
  v.text = "-9223372036854775808";
  assert(infer(v) == Type::BIGINT);
  Descriptor d;
  d.options.path = root + "/basic.csv";
  d.options.format = "csv";
  std::string error;
  int rc = infer_schema(d, root, error);
  if (rc) std::cerr << error << std::endl;
  assert(!rc);
  assert(d.columns.size() == 8 && d.columns[7].name == "id__2");
  assert(d.columns[4].type == Type::DATE && d.columns[5].type == Type::DATETIME &&
         d.columns[3].type == Type::BOOLEAN);
  Descriptor copy;
  assert(copy.decode(d.encode()));
  assert(copy.snapshot == d.snapshot);
  Scanner scan;
  assert(!scan.open(copy, root, {0, 1, 6}));
  std::vector<Cell> row;
  int n = 0;
  while ((rc = scan.next(row)) == 0) {
    if (!n) assert(row[6].text == "hello,\nworld");
    n++;
  }
  assert(rc == OB_ITER_END && n == 3);
  assert(!scan.rescan());
  assert(!scan.next(row) && row[0].text == "1");
  scan.close();
  d = Descriptor();
  d.options.path = root + "/basic.jsonl";
  d.options.format = "json";
  assert(!infer_schema(d, root, error));
  assert(d.columns.size() == 6 && d.columns[4].nullable);
  assert(!scan.open(d, root, {}));
  n = 0;
  while (!(rc = scan.next(row))) n++;
  assert(rc == OB_ITER_END && n == 3);
  scan.close();
  d = Descriptor();
  d.options.path = root + "/simple.parquet";
  d.options.format = "parquet";
  rc = infer_schema(d, root, error);
  if (rc) std::cerr << error << std::endl;
  assert(!rc);
  assert(!scan.open(d, root, {0}));
  n = 0;
  while (!(rc = scan.next(row))) {
    assert(row[0].text == std::to_string(n));
    n++;
  }
  if (rc != OB_ITER_END) std::cerr << scan.error() << std::endl;
  assert(rc == OB_ITER_END && n == 7);
  scan.close();
  d = Descriptor();
  d.options.path = root + "/basic.csv";
  d.options.format = "csv";
  assert(infer_schema(d, root + "/not-allowed", error) != 0);
  for (auto codec : {"NONE", "snappy", "gzip"})
    for (auto version : {"1.0", "2.0"}) {
      d = Descriptor();
      d.options.path = root + "/" + codec + "-" + version + ".parquet";
      d.options.format = "parquet";
      rc = infer_schema(d, root, error);
      if (rc) std::cerr << error << std::endl;
      assert(!rc);
      assert(!scan.open(d, root, {0, 1, 2, 3}));
      n = 0;
      while (!(rc = scan.next(row))) {
        if (n == 2)
          assert(row[0].null && row[1].null && row[2].null && row[3].null);
        else
          assert(row[0].text == std::to_string(n + 1));
        n++;
      }
      if (rc != OB_ITER_END) std::cerr << scan.error() << std::endl;
      assert(rc == OB_ITER_END && n == 5);
      scan.close();
    }
  d = Descriptor();
  d.options.format = "copy";
  d.options.path = root + "/export-utils.csv";
  d.options.header = true;
  d.columns = {{"a", Type::VARCHAR, true}, {"b", Type::VARCHAR, true}};
  unlink(d.options.path.c_str());
  CsvExporter exporter;
  assert(!exporter.open(d, root));
  Cell a;
  a.null = false;
  a.text = "NULL";
  Cell b;
  b.null = false;
  b.text = "say \"hi\"\n中文";
  assert(!exporter.write({a, b}));
  assert(!exporter.finish());
  assert(exporter.open(d, root) != 0);
  d.options.format = "csv";
  d.columns.clear();
  assert(!infer_schema(d, root, error));
  assert(!scan.open(d, root, {}));
  assert(!scan.next(row));
  assert(!row[0].null && row[0].text == "NULL" && row[1].text == b.text);
  scan.close();
  unlink(d.options.path.c_str());

  auto save = [&](const std::string &name, const std::string &text) {
    std::ofstream f(root + "/" + name, std::ios::binary);
    f << text;
  };
  auto schema = [&](const std::string &name, const std::string &format) {
    Descriptor x;
    x.options.path = root + "/" + name;
    x.options.format = format;
    int r = infer_schema(x, root, error);
    if (r) std::cerr << error << std::endl;
    assert(!r);
    return x;
  };
  save("edge.csv",
       "a,a,a__2,z,n\r\n1,true,x,9223372036854775808,NULL\r\n2,false,y,-9223372036854775809,\r\n");
  d = schema("edge.csv", "csv");
  assert(d.columns[1].name == "a__2" && d.columns[2].name == "a__2__2");
  assert(d.columns[3].type == Type::VARCHAR && d.columns[4].type == Type::NIL &&
         d.columns[4].nullable);
  save("union.jsonl", "\n{\"a\":1,\"b\":\"1\",\"c\":true}\n{\"b\":2.5,\"d\":null}\n");
  d = schema("union.jsonl", "json");
  assert(d.columns.size() == 4 && d.columns[0].nullable && d.columns[1].type == Type::VARCHAR &&
         d.columns[3].type == Type::NIL);
  for (auto bad : {"{\"a\":{}}\n", "{\"a\":[]}\n", "{\"a\":1,\"A\":2}\n", "{\"a\":1} garbage\n"}) {
    save("bad.jsonl", bad);
    d = Descriptor();
    d.options.path = root + "/bad.jsonl";
    d.options.format = "json";
    assert(infer_schema(d, root, error) != 0);
  }
  for (auto bad : {"a,b\n1\n", "a\n\"broken\n", "a\n\"x\"oops\n"}) {
    save("bad.csv", bad);
    d = Descriptor();
    d.options.path = root + "/bad.csv";
    d.options.format = "csv";
    assert(infer_schema(d, root, error) != 0);
  }
  save("change.csv", "a\n1\n");
  d = schema("change.csv", "csv");
  assert(!scan.open(d, root, {0}));
  save("change.csv", "a\n22\n");
  assert(scan.next(row) != 0);
  scan.close();
  assert(scan.open(d, root, {0}) != 0);
  scan.close();
  std::string link = root + "/escape.csv";
  unlink(link.c_str());
  assert(!symlink("/etc/passwd", link.c_str()));
  d = Descriptor();
  d.options.path = link;
  d.options.format = "csv";
  assert(infer_schema(d, root, error) != 0);
  unlink(link.c_str());
  // Deterministic corruptions exercise the real bounded metadata/page decoder
  // under ASan/UBSan, without assuming that every changed byte is invalid.
  std::ifstream original(root + "/simple.parquet", std::ios::binary);
  std::string parquet((std::istreambuf_iterator<char>(original)), std::istreambuf_iterator<char>());
  assert(parquet.size() > 32);
  uint32_t random = 0x4192ac32;
  for (int i = 0; i < 96; ++i) {
    std::string damaged = parquet;
    random = random * 1664525U + 1013904223U;
    if (i % 3 == 0) {
      damaged.resize(random % damaged.size());
    } else {
      damaged[random % damaged.size()] ^= char(1U << (i % 8));
    }
    save("mutated.parquet", damaged);
    Descriptor mutated;
    mutated.options.path = root + "/mutated.parquet";
    mutated.options.format = "parquet";
    if (infer_schema(mutated, root, error) == OB_SUCCESS &&
        scan.open(mutated, root, {0}) == OB_SUCCESS) {
      int rows = 0;
      while (scan.next(row) == OB_SUCCESS) {
        assert(++rows <= 10000);
      }
    }
    scan.close();
  }
  unlink((root + "/mutated.parquet").c_str());
  save("oversize.csv", "a\n" + std::string(MAX_RECORD + 1, 'x') + "\n");
  d = Descriptor();
  d.options.path = root + "/oversize.csv";
  d.options.format = "csv";
  assert(infer_schema(d, root, error) != OB_SUCCESS);
  unlink((root + "/oversize.csv").c_str());
  for (auto f : {"edge.csv", "union.jsonl", "bad.jsonl", "bad.csv", "change.csv"})
    unlink((root + "/" + f).c_str());
  std::cout << "Scanner schema, streaming, rescan, types, CSV, JSONL and Parquet checks passed"
            << std::endl;
}
