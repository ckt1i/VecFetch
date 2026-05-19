## 1. Benchmark Input Contract
- [x] 1.1 Add CLI support for an external ground-truth file to `benchmarks/bench_e2e.cpp`
- [x] 1.2 Load and validate the external GT before recall computation
- [x] 1.3 Record GT source metadata in `config.json` and `results.json`
- [x] 1.4 Keep the existing brute-force GT path as the fallback when no GT file is provided

## 2. MSMARCO Adapter
- [x] 2.1 Define the temporary adapter directory layout for MSMARCO
- [x] 2.2 Implement a script to map MSMARCO base/query embeddings into benchmark-compatible file names
- [x] 2.3 Generate stable passage/query id arrays for the adapter directory
- [x] 2.4 Emit benchmark-compatible metadata from MSMARCO passage text
- [x] 2.5 Copy or link the precomputed ground-truth artifact into the adapter directory

## 3. Benchmark Validation
- [x] 3.1 Run `bench_e2e` on the adapter directory with external GT enabled
- [x] 3.2 Verify recall and latency parity against the current COCO-style flow on a smoke slice
- [x] 3.3 Verify malformed or missing GT files fail fast with a clear error
- [ ] 3.4 Validate the adapter on MSMARCO-scale embeddings without brute-force GT recomputation

## 4. Cleanup and Documentation
- [x] 4.1 Document the temporary adapter workflow and GT file contract
- [x] 4.2 Note which parts of the adapter are temporary compatibility glue
- [x] 4.3 Leave the existing COCO benchmark path unchanged
