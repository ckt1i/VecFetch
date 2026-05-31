## 1. Baseline And Observability

- [ ] 1.1 Run the current baseline on `/home/zcq/VDB/test/data/MSMARCO/fht_kac_rotator` with real GT, `--skip-gt 0`, `--early-stop 0`, `--fine-grained-timing 0`, and unchanged search parameters; save command and JSON output.
- [x] 1.2 Add optional Stage1 fused-path counters to `ProbeStats` and `SearchStats`, such as fused block count, fused SafeOut lanes, and fused SafeIn lanes.
- [x] 1.3 Add matching `bench_e2e` `QueryResult`, `RoundMetrics`, pipeline JSON, and per-query sample fields without removing existing fields.
- [x] 1.4 Update benchmark logging to report fused Stage1 counters as diagnostics only, not as new tuning parameters.

## 2. Fused FastScan API

- [x] 2.1 Add `FastScanStage1EvalResult` and `FastScanStage1Evaluate` declarations in `include/vdb/simd/fastscan.h`.
- [x] 2.2 Implement AVX512 fused evaluation in `src/simd/fastscan.cpp`, computing distances, SafeOut mask, and optional SafeIn mask in the dequantize loop.
- [x] 2.3 Implement AVX2 and scalar fallback behavior that preserves correctness; it may call the legacy dequantize/mask functions if needed.
- [x] 2.4 Ensure `enable_safein=false` avoids SafeIn threshold computation and returns `safein_mask=0`.
- [x] 2.5 Preserve legacy `FastScanDequantize`, `FastScanSafeOutMask`, and `FastScanSafeInMask` as test oracle and fallback APIs.
- [x] 2.6 Use current interval-bound classification semantics: SafeOut threshold is `safeout_frontier_upper + safeout_margin_factor * norm`, SafeIn threshold is `safein_d_k - safein_margin_factor * norm`; do not use legacy `2 * margin` formulas.
- [x] 2.7 Preserve heap-not-full behavior by ensuring `safeout_frontier_upper=+inf` produces no SafeOut lanes.

## 3. RaBitQ And ClusterProber Integration

- [x] 3.1 Add a RaBitQ estimator helper or direct call path that performs `AccumulateBlock` followed by `FastScanStage1Evaluate`.
- [x] 3.2 Replace `ClusterProber::Probe` Stage1 hot path with the fused evaluation result.
- [x] 3.3 Keep `dists[32]` output semantics unchanged for Stage2 handoff, candidate batches, and CRC estimate buffering.
- [x] 3.4 Preserve fine-grained timing fields; Stage1 estimate/mask/iterate/classify fields must remain present and have clear diagnostic meaning.
- [x] 3.5 Keep the old Stage1 path available under tests or debug comparison so fused output can be compared against legacy output.
- [x] 3.6 Keep `CandidateBatch.est_error` and CRC estimate heap frontier plumbing unchanged; fused Stage1 must not reinterpret or recompute `safeout_frontier_upper`.

## 4. Correctness Tests

- [x] 4.1 Add SIMD unit tests comparing fused `out_dists` against `FastScanDequantize` for `count=1,7,16,31,32`.
- [x] 4.2 Add SIMD unit tests comparing fused SafeOut mask against `FastScanSafeOutMask` across multiple thresholds and margin factors.
- [x] 4.3 Add SIMD unit tests comparing fused SafeIn mask against `FastScanSafeInMask` when SafeIn is enabled, and verifying zero SafeIn mask when disabled.
- [x] 4.4 Cover representative dimensions including 512, 768, and 1024 where existing test utilities allow.
- [x] 4.5 Add or update query/ClusterProber tests to verify candidate classes and emitted candidates match the legacy Stage1 path.
- [x] 4.6 Add benchmark JSON smoke coverage to confirm new diagnostic fields are present and old fields remain present.
- [x] 4.7 Add boundary tests for one-sided interval formulas, including exact threshold equality, split safein/safeout margin factors, and `safeout_frontier_upper=+inf`.

## 5. Performance Validation

- [x] 5.1 Build affected targets, including SIMD tests, RaBitQ estimator tests, query scheduler tests, `test_ivf_index`, and `bench_e2e`.
- [x] 5.2 Run affected unit/regression tests: `test_ip_exrabitq`, FastScan/prepare-query related tests, `test_rabitq_estimator`, `test_overlap_scheduler`, `test_ivf_index`, and `test_rerank_consumer`.
- [x] 5.3 Before benchmarking, check CPU idle state and avoid parallel experiment runs.
- [x] 5.4 Run before/after benchmark on the MSMARCO `fht_kac_rotator` index with real GT, `--skip-gt 0`, `--early-stop 0`, and unchanged search parameters.
- [x] 5.5 Compare `recall@1/5/10`, `avg_query_time_ms`, `avg_probe_ms`, `avg_probe_stage1_ms`, `avg_probe_stage2_ms`, `avg_probe_submit_ms`, and fused Stage1 counters.
- [x] 5.6 Run diagnostic `--fine-grained-timing 1` only if needed to confirm Stage1 split movement; do not use fine-grained timing for final speed claims.
- [x] 5.7 Run delayed `perf record` after recall matches baseline and verify `RaBitQEstimator::EstimateDistanceFastScan` / Stage1 mask symbols decrease or move into the fused Stage1 path.
- [x] 5.8 Report observed speedup and whether it came from Stage1 pass fusion; if improvement is below noise, document that `AccumulateBlock` remains the next target.
