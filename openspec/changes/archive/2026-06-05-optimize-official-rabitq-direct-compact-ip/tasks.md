## 1. Layout Metadata and Storage Boundaries

- [x] 1.1 Add explicit optimized official `ex_bits=3` layout identifiers for `2-bit + 1-bit`, `1-bit + 1-bit + 1-bit`, and selected direct layout.
- [x] 1.2 Extend index metadata output to record `rabitq_estimator_mode`, `rabitq_total_bits`, `rabitq_ex_bits`, and the optimized layout key.
- [x] 1.3 Update cluster reader validation so generic v13 official bitstream, optimized `2-bit + 1-bit`, and optimized bitplane layouts cannot be silently confused.
- [x] 1.4 Keep the existing generic v13 decode-to-scratch path readable as fallback and validation input.

## 2. 2-bit + 1-bit Candidate Layout

- [x] 2.1 Implement writer-side packing for official `ex_bits=3` as low-2-bit 16B plus high-1-bit 8B per 64 dimensions.
- [x] 2.2 Extend resident parsed block views to expose `2-bit + 1-bit` compact pointers and strides without materializing decoded `uint8_t` ExData.
- [x] 2.3 Implement scalar reference decode/dot for `2-bit + 1-bit` compact chunks.
- [x] 2.4 Implement AVX512 direct compact masked IP for `2-bit + 1-bit` chunks.
- [x] 2.5 Add AVX2 or conservative fallback support for `2-bit + 1-bit` with identical output semantics.

## 3. 1-bit + 1-bit + 1-bit Candidate Layout

- [x] 3.1 Implement writer-side packing for official `ex_bits=3` as three 8B bitplanes per 64 dimensions.
- [x] 3.2 Extend resident parsed block views to expose three-bitplane compact pointers and strides without materializing decoded `uint8_t` ExData.
- [x] 3.3 Implement scalar reference decode/dot for three-bitplane compact chunks.
- [x] 3.4 Implement AVX512 direct compact masked IP for three-bitplane chunks using weighted plane accumulation.
- [x] 3.5 Add AVX2 or conservative fallback support for three-bitplane layout with identical output semantics.

## 4. Stage2 Query Integration

- [x] 4.1 Add a direct compact official Stage2 kernel dispatch interface keyed by optimized layout id.
- [x] 4.2 Route optimized official `ex_bits=3` Stage2 blocks through direct compact masked kernels instead of `ExRaBitQDecodePackedBatchBlockMagnitudes`.
- [x] 4.3 Preserve generic v13 official decode-to-scratch dispatch for fallback and correctness comparison.
- [x] 4.4 Ensure official score combination continues to use Stage1 `ip_x0_qr`, compact `ip_ex`, query `sum_q`, and official factor add/rescale.
- [x] 4.5 Extend benchmark JSON fields to report compact layout key, selected status, Stage2 decode counters, Stage2 kernel time, peak RSS, resident code bytes, and average rerank vectors.

## 5. Correctness and Microbench Coverage

- [x] 5.1 Add unit tests comparing `2-bit + 1-bit` direct IP against scalar decoded official ExData across random codes, dimensions, and lane masks.
- [x] 5.2 Add unit tests comparing three-bitplane direct IP against scalar decoded official ExData across random codes, dimensions, and lane masks.
- [x] 5.3 Add parity tests comparing optimized direct compact Stage2 output against generic v13 official fallback for the same sign-folded ExData codes.
- [x] 5.4 Add microbench coverage for both candidate kernels and record per-layout Stage2 kernel latency.

## 6. COCO100k Candidate Experiments

- [x] 6.1 Build a COCO100k optimized official `total_bits=4, ex_bits=3` index using the `2-bit + 1-bit` layout.
- [x] 6.2 Build a COCO100k optimized official `total_bits=4, ex_bits=3` index using the three-bitplane layout.
- [x] 6.3 Run the `2-bit + 1-bit` index with `nlist=2048`, `nprobe=64`, `topk=10`, `queries=1000`, and `non_safeout_candidate_budget=400`.
- [x] 6.4 Run the three-bitplane index with the same query set, GT, `nprobe`, `topk`, and candidate budget.
- [x] 6.5 Run or reuse the generic v13 official fallback result under the same controls for speedup comparison.
- [x] 6.6 Write a structured comparison summary with recall@10, avg latency, QPS, p95 latency, peak RSS, resident code bytes, Stage2 time, Stage2 decode counters, and average rerank vectors.

## 7. Winner Selection and Cleanup

- [x] 7.1 Apply the selection rule: correctness first, then lower avg latency, then peak RSS and implementation simplicity if latency differs by less than 5%.
- [x] 7.2 If neither candidate improves generic v13 avg latency by at least 20%, mark the fastest candidate experimental and do not replace main official `1+3` results.
- [x] 7.3 If a candidate passes acceptance, mark it as the selected official 3-bit direct compact layout in metadata and result summary.
- [x] 7.4 Remove the non-selected layout from default build/query routing or gate it behind an explicit experimental-only selection.
- [x] 7.5 Document final selected index path, rejected/experimental index path, and result paths for follow-up main-result reruns.

## 8. Final Validation

- [x] 8.1 Run targeted unit tests for ExRaBitQ packing, compact direct IP kernels, and Stage2 dispatch.
- [x] 8.2 Run `bench_e2e` on the selected layout and confirm recall remains stable against the official fallback.
- [x] 8.3 Verify benchmark output contains all required layout, memory, timing, and rerank-vector fields.
- [x] 8.4 Run `openspec status --change optimize-official-rabitq-direct-compact-ip` and confirm the change is apply-ready or complete.
