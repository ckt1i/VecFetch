## 1. Baseline And Existing Failure Review

- [x] 1.1 Re-run the current resident COCO benchmark on the existing parallel-view path and record the low-overhead and fine-grained Stage2 baseline.
- [x] 1.2 Confirm from code and benchmark output that the current `IPExRaBitQBatchPackedSignParallelCompact()` path is still lane-local in its main compute loop.
- [x] 1.3 Record the current `parallel_view_build_ms` and `parallel_view_bytes` as the resident-side baseline cost for comparison.

## 2. Compute-Ready Resident View Design

- [x] 2.1 Define the new resident `sign_pack` structure for each batch slice, including the minimal batch-friendly metadata needed for query-time sign preparation.
- [x] 2.2 Define the new resident `abs_pack` structure for each batch slice, including batch-consumable magnitude layout for the lane batch.
- [x] 2.3 Update the resident parsed-cluster / resident-cluster view structures so the new compute-ready packs can be exposed to query-time code.
- [x] 2.4 Keep the existing disk format unchanged and build the compute-ready packs only during preload / resident construction.

## 3. Preload-Time Transcode Upgrade

- [x] 3.1 Replace the current slice-friendly resident transcode with a compute-ready builder that emits `sign_pack` and `abs_pack`.
- [x] 3.2 Preserve fallback compatibility with the old compact path while making the new compute-ready resident data available when present.
- [x] 3.3 Keep resident build-time and resident-memory accounting intact for the upgraded compute-ready view.

## 4. Query-Time Sign Context Path

- [x] 4.1 Define the lightweight query-time `sign_ctx` representation derived from `sign_pack` and the current query slice.
- [x] 4.2 Implement the query-time batch sign-preparation phase that builds `sign_ctx` for a lane batch without materializing a large scratch buffer.
- [x] 4.3 Implement the query-time batch sign-apply phase that derives per-lane signed state and bias partials from `sign_ctx`.

## 5. Lane-Batched Kernel Update

- [x] 5.1 Rewrite the compute-ready Stage2 kernel so its primary hot path operates on lane batches rather than per-lane iteration.
- [x] 5.2 Implement the batch abs-consume phase using `abs_pack` and the batch signed state produced earlier in the kernel.
- [x] 5.3 Keep the tail path correct for non-full slices and non-full batches while preserving Stage2 scoring semantics.
- [x] 5.4 Ensure the updated kernel remains AVX-512-first and cross-dimension, without introducing a fixed-dimension serving path.

## 6. Integration And Verification

- [x] 6.1 Connect `ClusterProber` to the compute-ready resident view and the new batch kernel path.
- [x] 6.2 Verify equivalence against the existing compact Stage2 reference/debug checks.
- [x] 6.3 Re-run low-overhead resident benchmarks and confirm recall remains stable.
- [x] 6.4 Re-run fine-grained timing and confirm the new kernel path reduces at least one of `stage2_kernel_sign_flip` or `stage2_kernel_abs_fma`.
- [x] 6.5 Compare the new query-time gain against the resident-side `parallel_view_build_ms` and `parallel_view_bytes` cost and record the outcome.
