## Why

`bench_e2e` currently computes ground truth internally and assumes a COCO-style dataset layout. That makes it awkward to reuse for larger MSMARCO-scale inputs where the goal is to expose query hot-path bottlenecks, not to spend every run recomputing GT.

## What Changes

- Add an external ground-truth input path to `bench_e2e` so recall evaluation can reuse precomputed ANN ground truth instead of brute-force recomputation.
- Add a temporary MSMARCO-to-`bench_e2e` adapter workflow that maps MSMARCO embeddings and raw passage metadata into the directory contract expected by the benchmark.
- Keep the adapter intentionally narrow: it should support the minimum files needed for query execution, recall evaluation, and result logging.
- Preserve the existing COCO path unchanged; the new flow is additive and opt-in.
- Keep the GT input format simple and explicit so large datasets can be benchmarked without modifying query execution logic.

## Capabilities

### New Capabilities
- `bench-e2e-external-gt-input`: load precomputed ground truth from an explicit file instead of always brute-forcing it at runtime.
- `msmarco-bench-e2e-adapter`: generate a temporary COCO-style adapter directory from MSMARCO embeddings, ids, raw text, and precomputed GT files.

### Modified Capabilities
- None

## Impact

- Affects `benchmarks/bench_e2e.cpp` and its benchmark CLI / JSON output contract.
- Adds a small dataset-adapter script or utility under the benchmark/data-prep area.
- Enables MSMARCO-scale benchmark runs without forcing brute-force GT recomputation for every invocation.
- Leaves the existing COCO benchmark path and current query pipeline unchanged.
