# Implementation Notes

## Scope

Implemented the strong `serial_no_overlap` benchmark mode for resident RecordGate queries. The default behavior remains `overlap`.

## Decisions

- `QueryExecutionMode` was added to `SearchConfig`.
- `bench_e2e` / `bench_online_query.cpp` now accepts `--execution-mode overlap|serial-no-overlap`.
- Serial mode reuses the existing probe and read-plan generation path. `AsyncIOSink` remains the `ProbeResultSink`, but `ProbeResidentClusters()` skips every data I/O emission while `execution_mode=SerialNoOverlap`.
- After all probes finish, `ExecuteSerialDataReads()` consumes the collected read plans with synchronous reads:
  - combined store `VEC_ONLY`: direct `DataFileReader::ReadRaw()` of raw-vector bytes;
  - combined store `VEC_ALL`: direct `DataFileReader::ReadRaw()` of the full record, then `RerankConsumer::ConsumeAll()`;
  - separate store `VEC_ALL`: synchronous vector fd read plus synchronous payload fd read, because no single physical full-record range exists in NoCombine mode.
- Final missing payloads use `FetchMissingPayloadsSerial()` in serial mode, so no async payload `PrepRead/Submit/WaitAndPoll` path is used after top-k finalization.
- Batch exact rerank is preserved through `RerankConsumer::ExecuteBuffered()`.
- `budgeted_prefetch_limit` is forced to effective zero in benchmark serial mode, and scheduler-level speculative prefetch scheduling is also disabled when `execution_mode=SerialNoOverlap`.

## Reporting

Benchmark JSON now records:

- `execution_mode`
- `serial_no_overlap`
- `async_candidate_data_overlap_enabled`
- `serial_data_drains`
- `effective_budgeted_prefetch_limit`
- average serial vector/full-record/payload read timings, requests and bytes

The 20260705 run/summarize scripts write results under:

`/home/zcq/VDB/test/recordgate_serial_nooverlap_pipeline_ablation_20260705/`

## Validation

Build and tests passed:

- `cmake --build build --target bench_e2e test_overlap_scheduler -j2`
- `ctest --test-dir build -R 'test_(cluster_prober|overlap_scheduler|data_file|pread_fallback_reader|rerank_consumer|buffer_pool|payload_pipeline)' --output-on-failure`
- `openspec validate add-serial-nooverlap-pipeline-baseline --no-color`

Smoke and experiment runs:

- COCO smoke: `MODE=smoke`, 100 queries, topk 10/100, nprobe 64.
- Main formal: Amazon ESCI and MSMARCO, repeats 3, topk100/nprobe256 and topk10/nprobe128.
- Optional workload check: VoxCeleb2 and ImageNet1K, repeats 2, topk100/nprobe128, 200 queries, `SKIP_FALSE_STATS=1`.

All generated Full vs SerialNoOverlap pairs were valid under the summary tolerance, meaning recall, probed counts, SafeIn/SafeOut/Uncertain counts, rerank counts and read counts matched.

## Paper Interpretation

The main formal results do not support a strong independent pipeline contribution claim:

- Amazon ESCI: serial was roughly equal for topk10 and about 2% faster for topk100.
- MSMARCO: serial was roughly equal for topk10 and about 2% faster for topk100.
- VoxCeleb2 optional check showed serial about 1.5-2.6% slower.
- ImageNet1K optional check showed serial substantially faster in this 200-query check.

Therefore the current evidence suggests writing pipeline as an implementation mechanism or workload-sensitive appendix result, not as an independent main contribution.

