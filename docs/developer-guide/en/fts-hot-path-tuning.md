# FTS hot-path tuning

This change reduces repeated work in IK and basic English tokenization without
changing token boundaries, word frequencies, stopword rules or MATCH scoring.
It builds on the shared, read-only IK dictionaries in `ad7bfa5`.

## Bottlenecks and changes

CPU sampling during index construction showed `find_first_char_range` repeatedly
comparing a single UTF-8 character with dictionary range boundaries. IK also
walked a linked list of four processors and fetched the same current character
and character type inside each processor.

For UTF8MB4_BIN dictionaries, initialization now builds a direct table from BMP
codepoints to range dictionaries. Each table uses 512 KiB on a 64-bit host and
is read-only after the existing shared dictionary initialization publishes it.
The main and quantifier dictionaries together add 1 MiB per dictionary hub,
rather than per document. Overlapping ranges keep the first matching range.
Supplementary characters, invalid UTF-8, and other dictionary encodings or
collations retain the original range search.

IK keeps its four processors in a fixed array in the original order: letter,
quantifier, CJK, surrogate. The already-read character and type are passed to
each processor. Initialization failure and reset release each owned processor.

Word-frequency grouping now inserts or updates under one hash lookup and one
bucket lock. Updating an entry also replaces its spelling, preserving the old
behavior for distinct byte sequences that compare equal under the input
collation. The first occurrence still counts as one; subsequent occurrences
add the supplied frequency.

The basic English normalizer can write lowercase tokens into a caller-supplied
allocator. Only the BENG parser opts in, using its parser parameter allocator,
which also backed the old output copies. BENG can therefore return the token
without a second allocation and copy. Tokens remain valid after the normalizer
or iterator is destroyed. Other analyzer callers keep their existing temporary
storage behavior.

## Validation

On macOS arm64, the complete server executable built successfully. These
mysqltest cases compared successfully against their expected results:

- `fts_index/fts_hot_path`, including collation-equivalent spellings, repeated
  frequencies, long column values, Unicode, IK modes, BENG index construction,
  relevance, updates and deletes.
- The existing `fts_index/tokenize_function` and `fts_index/simple_query`.

A separate local differential run compared 6,070 input groups against the parent
implementation: the entire built-in main dictionary in smart and max-word
modes, every BMP scalar value, seven supplementary characters, seeded mixed
text, and boundary inputs. TOKENIZE JSON hashes or returned error numbers
matched for every group. The corpus also caught a first-spelling versus
last-spelling regression during development; both input orders now have focused
coverage in `fts_hot_path`.

The full 20,000-document workload preserved the exact IK and BENG token outputs
and query counts (8,001 / 11,000 / 7,332 / 20).

The complete FTS suite, Linux build and platform hidden tests have not been run
locally. `cn_word` requires cluster-admin credentials unavailable in the local
fixture. `basic_dml` previously failed on missing internal metadata fixture rows;
it is not counted as passing.

## Performance measurement

The comparison uses the same macOS arm64 host, isolated server configuration,
20,000 documents, batch size 500, 3,000 TOKENIZE rounds, 200 rounds per MATCH
query, three samples, 30 warmup rounds, and full data reload/index construction.
The local driver uses PyMySQL with the workload's data generator and SQL.

The build directory is named `build_release`, but its configured
RelWithDebInfo flags are actually `-O0 -g0 -DNDEBUG`. For this comparison, both
the parent and candidate rebuilt the same two unity translation units covering
FTS and text analysis with `-O2`; the rest of the executable retained `-O0`.
These are measurements of this mixed build, not a complete Release build or
the platform's optimized Linux environment. Earlier descriptions calling the
local configuration Release were inaccurate.

| Metric | Parent | Candidate | Time reduction |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 12.373960 | 11.232982 | 9.22% |
| `build_ik_content_sec` | 10.016076 | 9.070797 | 9.44% |
| `build_beng_en_sec` | 5.464983 | 5.310662 | 2.82% |
| `tokenize_ik_avg_ms` | 0.158619 | 0.143825 | 9.33% |
| `tokenize_beng_avg_ms` | 0.095809 | 0.094548 | 1.32% |
| `query_cn_avg_ms` | 2.834634 | 2.802526 | 1.13% |
| `query_beng_avg_ms` | 4.264306 | 4.257391 | 0.16% |
| `query_mixed_avg_ms` | 3.109799 | 3.134520 | -0.79% |
| `query_limit_avg_ms` | 3.578623 | 3.570619 | 0.22% |

Times are seconds for construction and milliseconds for tokenization/queries.
Category mean reductions: build 7.16%, tokenize 5.32%, query 0.18%. Equal-category mean: 4.22%.

The table compares only the incremental change against `ad7bfa5`. It does not
predict the contest score, whose baseline and hardware differ. Query execution
is unchanged, so small query timing differences should be treated as noise.

For the official workload on a dedicated instance, use the repository's
unmodified `tools/benchmark/fts_large_bench.sh` with `ROWS=20000 BATCH=500
ROUNDS=3000 QUERY_ROUNDS=200 SAMPLES=3 WARMUP=30 SKIP_LOAD=0`, and score its
report with the unmodified `fts_large_bench_score.py`. Keep hardware, compiler
flags and instance settings identical between revisions.


## Per-document word maps

`ObFTWordMap` now uses `NoPthreadDefendMode`. Its instances are local to a
synchronous TOKENIZE expression, index-row generation call, optimizer search
term extraction, or execution-time search term extraction. Each parallel worker
has a different map. The query token extraction paths use the shared type alias
rather than a separately spelled hash-map type, so they obey the same ownership
contract. No word map is published to another worker or retained in a shared
parser, dictionary or stopword checker. Shared dictionaries and the stopword
set retain their existing synchronization.

The default map allocated and initialized a latch per bucket and acquired that
latch for lookups and updates, even though the document's map has only one
caller. Removing these local latches keeps the same hash, equality, bucket
count, iteration order, frequency accumulation and last-spelling semantics.

The server was fully rebuilt after changing the map type, including its query
and data-plane callers. The same two FTS/text-analysis unity units were then
rebuilt at O2, as in the preceding comparison. Three mysqltests and all 6,070
local differential input groups passed again. A new regression,
`fts_index/include/fts_word_map_concurrency.py`, passed 400 concurrent TOKENIZE
comparisons on four connections, plus concurrent indexed writes with both
commit and rollback, followed by MATCH checks, update and delete. Run it on a
dedicated server with PyMySQL installed and `SEEKDB_TEST_PASSWORD` set.

The full workload was repeated twice for the candidate against a fresh
`8bf502e` baseline with the same mixed O0/O2 build settings.

| Metric | Parent | Candidate run 1 | Candidate run 2 |
| --- | ---: | ---: | ---: |
| `build_ik_all_sec` | 11.168307 | 11.125723 | 11.156654 |
| `build_ik_content_sec` | 9.003749 | 8.961952 | 8.905811 |
| `build_beng_en_sec` | 5.273625 | 5.311173 | 5.272805 |
| `tokenize_ik_avg_ms` | 0.143868 | 0.139197 | 0.142126 |
| `tokenize_beng_avg_ms` | 0.094803 | 0.089480 | 0.091604 |
| `query_cn_avg_ms` | 2.829125 | 2.772359 | 2.784706 |
| `query_beng_avg_ms` | 4.233128 | 4.240071 | 4.241337 |
| `query_mixed_avg_ms` | 3.122592 | 3.134622 | 3.117868 |
| `query_limit_avg_ms` | 3.568084 | 3.604730 | 3.558207 |

Category mean time reductions across the two candidate runs: build 0.22%, tokenize 3.36%, query 0.28%. Equal-category mean: 1.29%.
Each candidate run had three tokenization/query samples. Construction has
one sample per run. Small construction/query changes remain within observed
noise; the measured tokenization benefit is modest. This is not a platform
score prediction.
