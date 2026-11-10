# AI function regression

`ai_enhancement_regression.py` runs SQL against a freshly built, isolated seekdb
instance and a local HTTP mock. Install `pymysql`, start seekdb, then run:

```sh
SEEKDB_TEST_PASSWORD='your-test-instance-password' \
  python3 tools/deploy/mysql_test/test_suite/ai_function/include/ai_enhancement_regression.py \
  --host 127.0.0.1 --port 2883 --user root
```

The mock binds to a random loopback port. It does not call external models or
require model credentials. The runner creates a uniquely named test database,
models and endpoints, and removes them when finished. Use a disposable test
instance, since the test registers endpoints in the shared model namespace.

The 47 checks cover structured/nested/LOB prompts, repeated and invalid indices,
completion config and invalid responses, embedding dimensions and values,
Ollama native protocols with an empty key, stable reranking of 47 documents,
object IDs and malformed ranking results, and embedding scans with NULLs,
filters, LIMIT, varying models/dimensions, reversed responses and failed batches.
At least one actual HTTP embedding request must contain multiple SQL rows.

The competition's existing `t/ai_prompt.test` and
`t/ai_function_enhancement.test`, including their expected result files, are
unchanged. They also passed against the compiled server using a local mock with
the expected 768-dimensional vectors for the integration test.
