## 1. Baseline and Scope Lock

- [x] 1.1 Record the best pre-cleanup frozen-path metrics and command metadata from existing result documents into `/home/zcq/VDB/test/remove_redundant_query_features/baseline_reference.md`.
- [x] 1.2 Use code-review-graph first, then targeted text search, to inventory all symbols, flags, stats, tests, and benchmark fields tied to progressive pruning, Stage1 envelope skip, address sorting, and budgeted early submit.
- [x] 1.3 Confirm the existing validation indexes for the selected datasets are readable and record their paths; do not rebuild indexes unless a required index is missing or incompatible.
- [x] 1.4 Record the exact post-cleanup validation parameter set: dataset, index path, topk, nprobe, active ex-bits, resident ex-bits, two-level coarse routing flag, budget factor, query count, and output path.

## 2. Remove Stage1 Block Skip Envelope

- [x] 2.1 Remove Stage1 envelope fields from parsed cluster and resident cluster views.
- [x] 2.2 Remove Stage1 envelope preload/build code, resident byte accounting, and the `VDB_STAGE1_PRECOMPUTE_ENVELOPE` path from cluster storage.
- [x] 2.3 Remove Stage1 envelope skip functions, probe arguments, probe stats, and timing aggregation from cluster probing.
- [x] 2.4 Remove Stage1 envelope config propagation and stats aggregation from the overlap scheduler.
- [x] 2.5 Remove Stage1 envelope CLI flags, JSON fields, CSV fields, and benchmark summaries from `bench_online_query` and `bench_e2e`.
- [x] 2.6 Remove or rewrite tests that only validate Stage1 envelope skip behavior.

## 3. Remove Stage2 Progressive Pruning Surface

- [x] 3.1 Remove progressive active-bits and per-bit pruning config fields and stats from query search context.
- [x] 3.2 Remove progressive pruning constructor parameters, member variables, and branch logic from `ClusterProber`, leaving the fixed active ex-bits path intact.
- [x] 3.3 Remove progressive config propagation and stats aggregation from the overlap scheduler.
- [x] 3.4 Remove progressive pruning CLI flags, hidden compatibility aliases, JSON fields, CSV fields, and benchmark summaries from `bench_online_query` and `bench_e2e`.
- [x] 3.5 Keep low-level SIMD helper/kernel implementations unless compilation proves a helper is now truly unused and isolated; do not remove conservative SIMD code in this change.
- [x] 3.6 Update or remove tests that only exercise abandoned progressive query modes while keeping SIMD correctness tests that still compile.

## 4. Remove Abandoned Submit Scheduling Variants

- [x] 4.1 Remove vector-read address sorting config fields, CLI flags, benchmark metadata, and scheduler sort logic.
- [x] 4.2 Remove budgeted early submit config fields, scheduler plan queues, helper methods, call sites, stats, CLI flags, and benchmark metadata.
- [x] 4.3 Recheck vector-only submit, tail flush, final drain, SafeIn, SafeOut, and prefetch paths after removing the abandoned scheduler branches.
- [x] 4.4 Update tests that referenced address sorting or budgeted early submit so the frozen submit path remains covered.

## 5. Layout and Benchmark Surface Cleanup

- [x] 5.1 Preserve `tile_lane_bitmajor` and `vector_bitplanes` layout support and verify existing frozen and official-like baseline indexes still load.
- [x] 5.2 Remove user-visible references to abandoned experimental layout or pruning combinations from benchmark help text and experiment scripts, without deleting compatibility readers in this change.
- [x] 5.3 Update result parsers or plotting helpers that required removed feature-specific JSON fields.
- [x] 5.4 Ensure benchmark output still includes recall, avg latency or QPS, preload memory when applicable, coarse select time, probe prepare time, Stage1 time, Stage2 time, submit time, total probed count, and rerank or candidate count.

## 6. Build and Unit Validation

- [x] 6.1 Build the affected targets after cleanup.
- [x] 6.2 Run focused unit tests for cluster storage, cluster probing, query pipeline, benchmark argument parsing, and ExRaBitQ SIMD correctness.
- [x] 6.3 Run a focused smoke benchmark on a small or fast dataset to catch CLI/JSON/schema regressions before full validation.
- [x] 6.4 Fix any compile, test, or parser failures caused by stale removed fields.

## 7. Same-Index Performance Validation

- [x] 7.1 Run post-cleanup warm query validation on the selected frozen settings using existing indexes and write all raw logs/results under `/home/zcq/VDB/test/remove_redundant_query_features/`.
- [x] 7.2 Compare post-cleanup results against the recorded pre-cleanup best results for recall@10, avg_ms, QPS, avg_total_probed, and rerank or candidate counts.
- [x] 7.3 Treat the cleanup as passing only if recall@10 loss is at most `0.002`, avg_ms regression is at most `3%`, QPS drop is at most `3%`, and probed/candidate counts show no unexplained structural change above `1%`.
- [x] 7.4 If a metric exceeds tolerance, rerun the same point to separate noise from regression; if confirmed, isolate the removed feature group and revert or adjust that group.
- [x] 7.5 Write a final comparison report to `/home/zcq/VDB/test/remove_redundant_query_features/final_comparison.md` with commands, index paths, parameter settings, metrics, and pass/fail conclusion.

## 8. Final Documentation and OpenSpec Check

- [x] 8.1 Update any local optimization notes that still recommend the removed flags as viable frozen-path options.
- [x] 8.2 Run OpenSpec status/validation for `remove-redundant-query-features` and confirm the change is apply-ready.
- [x] 8.3 Summarize removed functionality, preserved conservative SIMD boundary, and validation outcome for the user.
