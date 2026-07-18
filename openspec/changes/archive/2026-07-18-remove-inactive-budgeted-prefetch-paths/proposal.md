## Why

The current no-cap RecordGate protocol no longer uses candidate-budget selection or its speculative raw-vector prefetch path, while SafeIn optional early-submit has failed to produce a stable end-to-end gain and remains disabled by default. Keeping these inactive branches in the scheduler, benchmark surface, statistics, and tests makes `VEC_ONLY`, `SPEC_VEC_ONLY`, and payload-prefetch behavior harder to reason about and creates avoidable risk that a stale experiment script silently changes the paper protocol.

## What Changes

- **BREAKING** Remove `non_safeout_candidate_budget` as a supported query control and make every candidate that survives SafeOut follow the normal exact-verification path without a separate global candidate cap.
- **BREAKING** Remove the associated speculative raw-vector prefetch feature, including `budgeted_prefetch_limit`, `SPEC_VEC_ONLY`, budget-maintenance heaps, speculative-vector caches, pending speculative queues, completion handling, counters, CLI flags, and benchmark fields.
- **BREAKING** Remove SafeIn optional early-submit, including `safein_optional_io_early_submit_max_requests`, the mandatory-backlog bypass branch, its counters, CLI flag, and benchmark fields.
- Preserve the current mandatory `VEC_ONLY` path, amplification-bounded vector-span coalescing, SafeOut, SafeIn payload/tail policy, resident fixed submit/flush/drain behavior, optional-I/O isolation, and final payload materialization.
- Preserve `SerialNoOverlap` as a benchmark-only pipeline ablation, but remove its now-redundant special handling for speculative vector prefetch.
- Update tests and maintained experiment scripts so removed flags fail clearly or are deleted rather than silently becoming no-ops.
- Validate the cleanup on reused indexes under the frozen no-cap protocol, requiring identical recall, probed-candidate counts, reranked-candidate counts, and read semantics within explicitly documented tolerances.

## Capabilities

### New Capabilities

- None.

### Modified Capabilities

- `query-pipeline`: Remove candidate-budget selection, speculative raw-vector requests, and SafeIn optional early-submit from the supported query state machine while preserving mandatory verification and payload materialization semantics.
- `payload-pipeline`: Require optional SafeIn payload I/O to obey the normal mandatory-backlog and optional-I/O scheduling policy without an early-submit bypass.
- `e2e-benchmark`: Remove the deleted controls and their dedicated output fields, and require same-index no-cap regression validation after cleanup.

## Impact

- Query configuration and statistics: `include/vdb/query/search_context.h`.
- Scheduler state and execution: `include/vdb/query/overlap_scheduler.h`, `src/query/overlap_scheduler.cpp`.
- Benchmark CLI, configuration metadata, JSON/CSV output, and aggregation: primarily `benchmarks/bench_online_query.cpp`, plus any maintained `bench_e2e` or analysis surface that still exposes the removed fields.
- Tests: scheduler tests for candidate budgets, speculative vector cache/in-flight consumption, SerialNoOverlap prefetch suppression, and optional early-submit.
- Experiment scripts and result parsers that pass or consume `--non-safeout-candidate-budget`, `--budgeted-prefetch-limit`, or `--safein-optional-io-early-submit-max-requests`.
- Existing indexes and on-disk record formats are unaffected; no index rebuild or data migration is required.
