## 1. Baseline And Observability

- [ ] 1.1 Add `ProbeStats` fields for Stage2 masked-kernel calls, requested lanes, skipped lanes, and total valid lanes.
- [ ] 1.2 Add matching `SearchStats`, `bench_e2e` `QueryResult`, `RoundMetrics`, pipeline JSON, and per-query sample fields without removing existing fields.
- [ ] 1.3 Update benchmark logging to report Stage2 lane density only as diagnostic output, not as a new tuning parameter.
- [ ] 1.4 Run the current baseline with real GT, `--skip-gt 0`, `--early-stop 0`, and `--fine-grained-timing 0`; save JSON and command line for before/after comparison.

## 2. Mask-Aware Kernel API

- [ ] 2.1 Add `IPExRaBitQBatchPackedSignParallelCompactMasked` declaration in `include/vdb/simd/ip_exrabitq.h`.
- [ ] 2.2 Implement the AVX512 masked compact kernel in `src/simd/ip_exrabitq.cpp`, preserving output lane indexes and only writing selected lanes.
- [ ] 2.3 Add a full-mask fast path that calls the existing full-lane kernel or otherwise avoids masked overhead when `lane_mask` covers all valid lanes.
- [ ] 2.4 Keep non-AVX512 or unsupported builds compiling with the existing fallback behavior.

## 3. ClusterProber Integration

- [ ] 3.1 In compact v11 parallel layout, call the masked kernel from `ClusterProber::Probe` using `block.lane_mask`.
- [ ] 3.2 Preserve the existing full-lane kernel path for non-parallel layout, unsupported storage versions, and fallback cases.
- [ ] 3.3 Accumulate Stage2 lane utilization stats from each processed block.
- [ ] 3.4 Ensure scatter/classification continues to consume `block.lane_mask` and that unselected `out_ip_raw` lanes are never read.

## 4. Correctness Tests

- [ ] 4.1 Add SIMD unit tests comparing masked-kernel outputs against the full-lane kernel for randomized masks, full masks, single-lane masks, and empty masks.
- [ ] 4.2 Cover dimensions representative of `n * 32` / `n * 64`, including 512, 768, and 1024 where test utilities allow.
- [ ] 4.3 Add or update `ClusterProber` tests to verify compact v11 masked path produces the same candidate classes as the existing full-lane path.
- [ ] 4.4 Add benchmark JSON schema smoke coverage to confirm new statistics are present and old fields remain present.

## 5. Performance Validation

- [ ] 5.1 Build affected targets, including SIMD tests, cluster prober tests, `test_ivf_index`, and `bench_e2e`.
- [ ] 5.2 Before benchmarking, check CPU idle state and avoid parallel experiment runs.
- [ ] 5.3 Run before/after benchmark on `/home/zcq/VDB/test/data/MSMARCO/fht_kac_rotator` with real GT, `--skip-gt 0`, `--early-stop 0`, and unchanged search parameters.
- [ ] 5.4 Compare `recall@1/5/10`, `avg_query_time_ms`, `avg_probe_ms`, `avg_probe_stage2_kernel_ms` when fine-grained timing is used diagnostically, `avg_probe_submit_ms`, and Stage2 lane utilization stats.
- [ ] 5.5 Run delayed `perf record` only after recall matches baseline, and verify samples in `IPExRaBitQBatchPackedSignParallelCompact` decrease or move into the masked kernel with lower total query time.
- [ ] 5.6 Report whether the observed speedup comes from skipped Stage2 lanes; if lane density is too high for benefit, document that result and do not claim a default recommendation.
