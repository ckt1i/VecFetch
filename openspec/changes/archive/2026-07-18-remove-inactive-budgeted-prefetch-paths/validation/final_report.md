# Final validation report

Date: 2026-07-17

## Outcome

PASS. The inactive candidate-budget/speculative-vector path and SafeIn optional
early-submit bypass were removed without changing query semantics or supported
I/O work. A same-host interleaved pre/post comparison did not show a consistent
performance regression, so the user-requested rollback gate was not triggered.

## Build and focused tests

Built targets:

- `bench_e2e` (online query entry point)
- `bench_build_index` (legacy full-flow entry point)
- `test_overlap_scheduler`

Validation commands:

```text
cmake -S . -B build
cmake --build build --target bench_e2e bench_build_index test_overlap_scheduler -j4
ctest --test-dir build --output-on-failure \
  -R 'test_overlap_scheduler|test_bench_e2e_rejects_deleted_prefetch_flags'
```

Result: 2/2 CTest targets passed. The scheduler target ran its full suite. The
benchmark argument test exercised all three deleted flags against both maintained
benchmark entry points and required non-zero exit plus the migration message.

Modified Python result summarizers were parsed with `ast.parse`; modified shell
runners passed `bash -n`.

## Same-index regression protocol

Dataset/point: MSMARCO Passage, `topk=100`, `nprobe=96`, 500 holdout queries.

Shared assets:

- index/store: `/home/zcq/VDB/test/recordgate_vec_span_stage1_20260715/stores/msmarco_passage/compact_g4_prefix0_align16`
- queries/ids/GT: `/home/zcq/VDB/test/recordgate_safein_confidence_bextra_20260712/inputs/msmarco_passage/holdout`
- epsilon cache: `/home/zcq/VDB/test/recordgate_hybrid_split_static_safein_20260712/runtime_epsilon_cache/msmarco_passage_safeout_p0p99_s100_legacy_per_cluster.txt`
- frozen pre-cleanup binary: `/home/zcq/VDB/test/recordgate_p0_p1_cpu_memory_autoreview_20260717/bin/bench_e2e_after`
- post-cleanup binary: `/home/zcq/VDB/VectorRetrival/build/benchmarks/bench_e2e`

The exact formal sequence was preserved per repeat: 500-query warmup followed by
`NoSafeIn, SafeIn` on odd repeats and `SafeIn, NoSafeIn` on even repeats. Formal
runs used `drop-before-queries`, CPU 8, shared submission, queue depth 64, 1024
fixed vector buffers, 64 KiB/1.5x spans, payload reuse, and eight SafeIn tails.
The three deleted zero-valued CLI controls were omitted.

Artifacts:

- historical pre reference: `/home/zcq/VDB/test/recordgate_p0_p1_cpu_memory_autoreview_20260717/runs/formal_sweep/msmarco_passage/k100/np96/SafeIn`
- post exact-protocol repeats: `/home/zcq/VDB/test/remove_inactive_budgeted_prefetch_paths_20260717/final_regression_v1`
- same-host frozen-pre repeats: `/home/zcq/VDB/test/remove_inactive_budgeted_prefetch_paths_20260717/concurrent_pre_regression_v1`
- interleaved A/B pairs: `/home/zcq/VDB/test/remove_inactive_budgeted_prefetch_paths_20260717/paired_ab_v1`

## Semantic and supported-I/O comparison

The frozen-pre and post-cleanup pair produced identical values:

| Metric | Pre | Post |
| --- | ---: | ---: |
| Recall@100 | 0.868660 | 0.868660 |
| avg probed candidates | 61324.478 | 61324.478 |
| avg reranked candidates | 1022.126 | 1022.126 |
| avg vector requests | 326.582 | 326.582 |
| avg vector bytes | 3882349.824 | 3882349.824 |
| avg span requests | 84.450 | 84.450 |
| avg span bytes | 3138520.320 | 3138520.320 |
| avg payload requests | 53.252 | 53.252 |
| avg payload bytes | 54947.840 | 54947.840 |
| avg span payload reuse hits | 46.748 | 46.748 |
| avg SafeIn tails extended | 6.116 | 6.116 |
| avg optional-I/O submitted | 0 | 0 |
| avg final payload I/O prepared | 0 | 0 |

The unit suite additionally verifies result equality between overlap and
`SerialNoOverlap`, Uncertain accounting, mandatory request conservation, vector
span completion, optional-I/O blocking/resumption, and final payload completion.

## Performance rollback gate

Comparing the new run only with the older historical run suggested a 2.42%
latency increase (historical median 2.722577 ms versus post median 2.788442 ms).
That comparison was rejected as a causal signal because a same-host rerun of the
unchanged frozen binary had drifted to a 2.927950 ms median.

Four same-host interleaved A/B pairs were then run with binary order alternated:

| Pair | Frozen pre (ms) | Post cleanup (ms) | Post direction |
| --- | ---: | ---: | --- |
| 1 | 2.785874 | 2.784925 | faster |
| 2 | 2.793993 | 2.805766 | slower |
| 3 | 2.850224 | 2.878836 | slower |
| 4 | 2.934610 | 2.918681 | faster |

Two pairs favored each binary. The median paired relative delta was about
`+0.19%` for post cleanup, with mixed signs and much smaller magnitude than the
observed time-of-run drift. This is treated as performance-neutral, not as a
speedup claim and not as a confirmed regression. If a later multi-dataset paired
run establishes a repeatable degradation, the change must be reverted atomically
as requested; no partial restoration of speculative state is acceptable.

## CLI and schema migration

New results omit all dedicated fields for:

- non-SafeOut candidate budget
- budgeted/speculative vector prefetch
- SafeIn optional early-submit

The frozen pre result intentionally retains these historical fields. Current
summarizers no longer emit them, while historical JSON remains readable because
parsers access independent supported fields without requiring the removed keys.

## Residual-symbol audit

Literal audit over `include`, `src`, `tests`, and `benchmarks` found no runtime,
state, statistics, or schema reference to:

- `non_safeout_candidate_budget`
- `budgeted_prefetch_limit`
- `SPEC_VEC_ONLY`
- `budgeted_prefetch_`
- `safein_optional_io_early_submit`
- speculative queue/cache/offset symbols

Allowed residues are limited to:

- explicit deleted-flag rejection strings and their CMake test
- this OpenSpec change and older OpenSpec/history records
- dated optimization/result evidence explicitly marked historical/non-runnable

## OpenSpec validation

```text
openspec validate remove-inactive-budgeted-prefetch-paths --strict
```

Result: valid.
