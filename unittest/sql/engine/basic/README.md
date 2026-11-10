# File SQL scanner regression

This standalone test exercises the production scanner and CSV exporter directly,
with assertions enabled. It does not require a database process. Python/PyArrow
is used only to create test inputs; production File SQL uses the native C++ scanner.

From the repository root, with initialized third-party dependencies:

```sh
python3 unittest/sql/engine/basic/generate_file_scan_fixtures.py /tmp/file-sql-test-data
c++ -std=c++17 -g -fsanitize=address,undefined -I src -I src/oblib \
  -I deps/3rd/usr/local/oceanbase/deps/devel/include \
  unittest/sql/engine/basic/test_file_scan_utils.cpp \
  src/sql/engine/basic/ob_file_scan_utils.cpp -lz -o /tmp/test-file-scan
/tmp/test-file-scan /tmp/file-sql-test-data
```

On macOS, select a compatible SDK with `-isysroot` if required by the local toolchain.
Use a disposable writable fixture directory: tests create and remove their own edge
case files inside it. Do not compile with `NDEBUG` because assertions are the test
checks.

Database-level coverage lives in
`tools/deploy/mysql_test/test_suite/file_sql/include/file_sql_regression.py`.
Invoke its `run(connection_dictionary, disposable_secure_root)` against a test
server. Create `file_sql_regression` first, set `secure_file_priv` to that root
through a local privileged connection, and install PyMySQL/PyArrow in the test
client environment. The supplied connection should use the local socket so
security-setting regressions can run. Keep the production deployment separate.
The official `file_sql_basic.test` and `.result` are unchanged.
