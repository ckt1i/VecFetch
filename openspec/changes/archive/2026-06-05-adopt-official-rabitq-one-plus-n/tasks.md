## 1. Baseline Audit

- [x] 1.1 Identify all current uses of `RaBitQConfig.bits` in build, storage, query, calibration, and benchmark code.
- [x] 1.2 Document current legacy signed-magnitude fields: Stage1 packed codes, Stage2 magnitude, `ex_sign_packed`, `xipnorm`, and metadata.
- [x] 1.3 Add a short implementation note mapping legacy v10/v11/v12 formats to the new official v13 format.

## 2. Configuration And Metadata

- [x] 2.1 Extend RaBitQ config metadata to represent `total_bits`, `ex_bits`, and estimator mode explicitly.
- [x] 2.2 Preserve legacy `bits` loading behavior for existing indexes and map it to `legacy_signed_magnitude`.
- [x] 2.3 Update metadata serialization to write `rabitq_total_bits`, `rabitq_ex_bits`, `rabitq_estimator_mode`, and `.clu` storage version for official indexes.
- [x] 2.4 Update benchmark/result metadata to report both total and ex bits.
- [x] 2.5 Add validation that official indexes satisfy `total_bits == ex_bits + 1`.

## 3. Official ExData Encoding

- [x] 3.1 Implement official sign-folded ExData quantization for `ex_bits > 0`.
- [x] 3.2 Complement ExData codes for negative residual dimensions following official RaBitQ semantics.
- [x] 3.3 Compute and store official Stage2 factors required by the official estimator.
- [x] 3.4 Support `total_bits=1` as `ex_bits=0` with no Stage2 ExData payload.
- [x] 3.5 Keep legacy signed-magnitude encoding available for old format builds and comparison runs.
- [x] 3.6 Add scalar reference tests comparing official code generation against the third-party RaBitQ convention on small inputs.

## 4. v13 Storage Layout

- [x] 4.1 Define a new `.clu` storage version for official `1+n` ExData.
- [x] 4.2 Implement v13 writer support for batch-major blocked sign-folded ExData.
- [x] 4.3 Ensure v13 writer does not persist `ex_sign_packed`.
- [x] 4.4 Implement v13 reader parsing and expose official ExData through `ParsedCluster`.
- [x] 4.5 Preserve v10/v11/v12 reader behavior for legacy indexes.
- [x] 4.6 Reject incompatible format/estimator-mode combinations with clear errors.
- [x] 4.7 Add pack/unpack round-trip tests for `ex_bits=2`, `ex_bits=3`, and `ex_bits=4`.

## 5. Resident And Query Views

- [x] 5.1 Update compact resident preload to retain v13 ExData in packed form.
- [x] 5.2 Ensure resident state does not materialize full-index decoded official ExData.
- [x] 5.3 Update resident component-byte metrics to include official ExData storage bytes.
- [x] 5.4 Add query scratch support for touched official ExData blocks where decode-to-scratch is needed.
- [x] 5.5 Verify resident parsed views do not reference temporary preload buffers after preload completes.

## 6. Stage1 To Stage2 Data Flow

- [x] 6.1 Expose `ip_x0_qr` or an equivalent raw Stage1 accumulator for each Stage2 candidate.
- [x] 6.2 Store the Stage1 intermediate value in Stage2 lane metadata without changing candidate ids or lane masks.
- [x] 6.3 Add scalar diagnostics validating Stage1-derived `ip_x0_qr` against an independent reference computation.
- [x] 6.4 Keep legacy Stage2 path independent of the new `ip_x0_qr` requirement.

## 7. Official Stage2 Kernels

- [x] 7.1 Implement scalar official ExData score computation.
- [x] 7.2 Implement or adapt SIMD kernels for `ex_bits=2` and `ex_bits=4`.
- [x] 7.3 Implement `ex_bits=3` support, using either direct SIMD dot or measured decode-to-scratch fallback.
- [x] 7.4 Combine `2^ex_bits * ip_x0_qr + ip_ex + factor terms` in the Stage2 score path.
- [x] 7.5 Add parity tests between scalar and SIMD/fallback kernels across full lanes, masked lanes, and tail dimensions.
- [x] 7.6 Preserve legacy `IPExRaBitQBatchPackedSign*` behavior for legacy indexes.

## 8. Classification And Calibration

- [x] 8.1 Update SafeIn/SafeOut Stage2 classification to use official estimates for official indexes.
- [x] 8.2 Ensure ConANN epsilon and margin formulas use `total_bits` for official indexes.
- [x] 8.3 Ensure Stage2 packing/kernel code uses `ex_bits`, not `total_bits`.
- [x] 8.4 Update RabitQ-space SafeIn d_k and candidate CRC calibration to record official estimator mode.
- [x] 8.5 Add fallback handling for legacy indexes without official metadata.

## 9. CLI And Benchmark Integration

- [x] 9.1 Add build-time controls for selecting official `1+n` vs legacy signed-magnitude mode.
- [x] 9.2 Add query-time auto-detect and explicit official/legacy validation modes.
- [x] 9.3 Update `bench_e2e` and `bench_online_query` outputs with official format metadata.
- [x] 9.4 Prevent result aggregation from merging legacy `bits=4` with official `total_bits=4`.
- [x] 9.5 Write official rebuild outputs to distinct directories by default.

## 10. Validation And Experiments

- [x] 10.1 Run unit tests for storage version parsing, metadata validation, and pack/unpack round trips.
- [x] 10.2 Run scalar/SIMD official score parity tests on synthetic vectors.
- [x] 10.3 Build COCO100k official indexes for target total bits and record index size.
- [x] 10.4 Run COCO100k memory, latency, recall, SafeOut, and rerank-count probes.
- [x] 10.5 Compare official `total_bits=4` with legacy `bits=4` and document expected recall/QPS/memory differences.
- [x] 10.6 If COCO100k passes parity and recall targets, extend validation to Amazon ESCI and the remaining Pareto datasets.
