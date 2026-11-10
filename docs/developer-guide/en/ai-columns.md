# Asynchronous AI columns

MySQL mode supports physical AI result columns computed by `AI_COMPLETE` and
`AI_EMBED`. Register the corresponding AI models and endpoints first. Use an
explicit primary key for AI column tables.

```sql
CREATE TABLE docs (
  id BIGINT PRIMARY KEY,
  content VARCHAR(2000),
  summary VARCHAR(2000) AI COLUMN (AI_COMPLETE('text_model', content))
    WITH (sync_mode='async', max_retries=2),
  emb VECTOR(4) AI COLUMN (AI_EMBED('embed_model', content))
    WITH (sync_mode='async', max_retries=2)
);
INSERT INTO docs(id, content) VALUES (1, 'An example document');
SHOW AI COLUMNS FROM docs;
SHOW CREATE TABLE docs;
ALTER TABLE docs RETRY AI COLUMN summary WHERE status = 'FAILED';
SET SESSION ai_column_strict_read = ON;
```

`AI_COMPLETE` supports string, integer and JSON result columns. Integer and JSON
results receive type-specific instructions and are validated before publication.
Embedding dimensions are derived from the VECTOR column and checked against an
explicit third argument when present. Only asynchronous mode is supported;
`max_retries` is in the inclusive range 0–16. AI-to-AI dependencies, subqueries,
session variables, dynamic model names and nested model calls are rejected.
Business statements cannot assign AI result or internal state columns. Retry
`status` is a pseudo-column; backtick-quoted `status` remains a business column.

The schema retains normalized expressions and dependencies in a reserved column
comment, which follows existing schema copying and serialization. Three hidden
columns hold each result's state, request token and definition fingerprint.
`oceanbase.__all_ai_column` exposes definitions; `__all_ai_column_task` stores
pending work, leases, retry counts and intermediate results.

Server-generated native row triggers invalidate results and maintain the queue
in the same storage transaction as business writes. Rollback also rolls back
invalidation and enqueueing. Primary-key moves and deletes remove obsolete
work. Four native background threads claim durable row/request groups, evaluate
native AI expressions after commit, then publish all terminal columns together
in one transaction. Publication checks request tokens, definition fingerprints,
current table identity and locked lease ownership. Superseded or deleted rows
cannot be restored by a delayed response. A restarted server reclaims leases
from its previous process; a lost response can still require another upstream
call, so model execution is not exactly once.

Pending and failed results are NULL. Strict reads inspect every referenced AI
column across the whole table, including references in predicates and writes.
An unfinished or failed row raises error 11120 even if a query filters it out.
Ordinary column access remains available. AI statements are resolved on each
execution, including prepared statements, so readiness is not cached. Adding
an AI column backfills existing rows; modifying a definition changes its
fingerprint and recomputes old results. Dropping or renaming a surviving AI
column's dependency is rejected.

Physical result updates use native index maintenance. BEFORE UPDATE triggers
can invalidate a vector absent from the explicit SET list; the resolver also
refreshes its internal vector identifier in that case. This avoids duplicate
HNSW delta-buffer keys when a NULL vector is regenerated.

## Validation

The five original `tools/deploy/mysql_test/test_suite/ai_column` mysqltests and
expected results are unchanged and pass with the implementation:

- `ai_column_ddl`
- `ai_column_t1_dml`
- `ai_column_worker`
- `ai_column_retry_strict`
- `ai_column_vector_index`

`ai_column_boundary` adds existing-row backfill, result ownership, ordinary DML
under strict reads, and last-AI-column removal. `include/ai_column_regression.py`
adds definition modification, rename, prepared strict reads, invalid typed
outputs, NULL prompts, escaped composite primary keys, and seed/resume phases
for restart recovery. The seed and resume phases use the same isolated database
directory across a server restart. All HTTP services are local mocks.

Local verification uses a complete macOS arm64 server build. It does not establish
Linux build results, hidden-test scores, performance scores or real-provider
behavior. AI installation creates metadata and triggers after native table DDL;
the whole installation is not one atomic schema transaction. Concurrent DDL and
DML and interruption during that installation need additional coordination.
