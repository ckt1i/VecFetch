## 1. Configuration and Benchmark Entry

- [x] 1.1 Add a `QueryExecutionMode` enum with `Overlap` and `SerialNoOverlap` to query configuration.
- [x] 1.2 Add `--execution-mode overlap|serial-no-overlap` to `bench_online_query`, defaulting to `overlap`.
- [x] 1.3 Write `execution_mode`, effective `budgeted_prefetch_limit`, async-overlap status and existing `serial_data_drains` status to benchmark JSON.
- [x] 1.4 Ensure existing benchmark commands without `--execution-mode` remain behavior-compatible with the current overlap path.

## 2. Candidate Collection Path

- [x] 2.1 Factor shared candidate scan, dedup, truth-stat, SafeIn/Uncertain partition and frontier-estimate buffering out of the current async sink where practical.
- [x] 2.2 Add a collecting `ProbeResultSink` that receives `CandidateBatch` objects and records read plans without issuing data I/O.
- [x] 2.3 Preserve dynamic SafeOut frontier updates and dynamic SafeIn state transitions in serial mode.
- [x] 2.4 Preserve `non_safeout_candidate_budget` heap selection and final read-plan materialization semantics in serial mode.
- [x] 2.5 Disable or reject speculative `budgeted_prefetch_limit` in serial mode and verify no `SPEC_VEC_ONLY` read plans can be emitted.

## 3. Serial NoOverlap Query Execution

- [x] 3.1 Add `SearchSerialNoOverlap()` or an equivalent execution-mode branch in `OverlapScheduler::Search()`.
- [x] 3.2 Implement probe-first behavior: coarse routing and all selected cluster probes complete before any candidate data read is issued.
- [x] 3.3 Implement synchronous `VEC_ONLY` reads using `DataFileReader::ReadRaw` or an equivalent direct `pread` helper.
- [x] 3.4 Implement synchronous `VEC_ALL` full-record reads preserving `RerankConsumer::ConsumeAll()` payload-cache semantics.
- [x] 3.5 Execute exact rerank after candidate data reads using the existing `RerankConsumer::ExecuteBuffered()` path.
- [x] 3.6 Implement synchronous final payload reads after top-k finalization while preserving combined-store and, if practical, separate-store behavior.
- [x] 3.7 Ensure `FinalDrain` is not used for serial candidate data and that serial mode leaves no in-flight data I/O.

## 4. Statistics and Reporting

- [x] 4.1 Add serial timing stats for raw-vector read, full-record read and final payload read.
- [x] 4.2 Add or map serial read request/byte counters so they remain comparable to overlap path `vec_only`, `all_read` and `payload` counters.
- [x] 4.3 Preserve existing recall, total probed, SafeOut/SafeIn/Uncertain, stage1/stage2, rerank and read-byte fields.
- [x] 4.4 Update benchmark JSON writing and CSV summaries to include execution-mode and serial attribution fields.
- [x] 4.5 Update pipeline-ablation summary scripts to mark Full vs Serial NoOverlap pairs invalid when recall or mechanism counts mismatch beyond tolerance.

## 5. Tests and Correctness Checks

- [x] 5.1 Add unit or integration coverage for `execution_mode=serial_no_overlap` on a small resident index.
- [x] 5.2 Verify serial mode emits no async candidate-data read before probe completion.
- [x] 5.3 Verify Full Pipeline and Serial NoOverlap return identical or tolerance-equivalent recall and final result ordering on a deterministic small dataset.
- [x] 5.4 Verify SafeIn `VEC_ALL` serial reads cache payloads and final missing payload reads only fetch uncached final top-k payloads.
- [x] 5.5 Verify `budgeted_prefetch_limit` is effectively disabled or rejected under serial mode.

## 6. Build and Smoke Validation

- [x] 6.1 Build affected targets, including `bench_online_query` and query/storage tests.
- [x] 6.2 Run focused unit tests for cluster probing, overlap scheduler, data reader and rerank consumer behavior.
- [x] 6.3 Run a COCO100K smoke benchmark with existing index: `topk=10,nprobe=64,queries=100`, overlap vs serial no-overlap.
- [x] 6.4 Confirm smoke pair matches recall, total probed, SafeOut/SafeIn/Uncertain, rerank count and read counts before running larger experiments.

## 7. Experiment Script Integration

- [x] 7.1 Add or update a pipeline ablation run script for strong Full Pipeline vs Serial NoOverlap selected points.
- [x] 7.2 Keep old `serial_data_drains` results labeled as weak serial-drain diagnostics, not strong No Pipeline.
- [x] 7.3 Write raw results under `/home/zcq/VDB/test/recordgate_serial_nooverlap_pipeline_ablation_20260704/` or a new dated sibling path.
- [x] 7.4 Produce a summary table with avg/p50/p95/p99 latency, QPS, recall, mechanism-count deltas, serial read timings and valid/invalid attribution flags.

## 8. Final Review

- [x] 8.1 Run `openspec validate add-serial-nooverlap-pipeline-baseline`.
- [x] 8.2 Document implementation decisions that differ from the proposal, especially any unsupported separate-store behavior.
- [x] 8.3 Summarize whether Serial NoOverlap provides enough evidence to keep pipeline as an independent paper contribution.
