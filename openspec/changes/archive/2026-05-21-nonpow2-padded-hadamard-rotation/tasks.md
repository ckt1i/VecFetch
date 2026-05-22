## 1. Metadata And Configuration

- [x] 1.1 Add `logical_dim` / `effective_dim` modeling to IVF-RaBitQ build metadata and reopen metadata parsing
- [x] 1.2 Add explicit `padding_mode` and `rotation_mode` metadata fields for baseline and padded-Hadamard indexes
- [x] 1.3 Add build/query configuration switches to enable padded-Hadamard experimentation without changing the default path

## 2. Build Path

- [x] 2.1 Implement zero-padding from `logical_dim` to `effective_dim = next_power_of_two(logical_dim)` in the padded-Hadamard build path
- [x] 2.2 Extend the build pipeline to generate rotated centroids in `effective_dim` space for padded-Hadamard indexes
- [x] 2.3 Persist padded-Hadamard artifacts and verify reopen compatibility with the resulting index directory

## 3. Query Pipeline

- [x] 3.1 Pad non-power-of-two query vectors from `logical_dim` to `effective_dim` at query start when padded-Hadamard metadata is enabled
- [x] 3.2 Route padded-Hadamard queries through query-once Hadamard rotation and `PrepareQueryRotatedInto`
- [x] 3.3 Preserve the baseline random-rotation path for non-padded indexes and verify result compatibility

## 4. Benchmark And Observability

- [x] 4.1 Extend benchmark output with `logical_dim`, `effective_dim`, `padding_mode`, and `rotation_mode`
- [x] 4.2 Add prepare attribution fields for rotation vs quantize/LUT cost in benchmark output
- [x] 4.3 Add index-size or artifact-size reporting needed to compare padded-Hadamard footprint against baseline

## 5. Experiment Workflow

- [x] 5.1 Add a prepare/rotation microbenchmark or equivalent prepare-focused comparison for baseline vs padded-Hadamard
- [x] 5.2 Add a full E2E comparison workflow for MSMARCO under the fixed operating point (`nlist=16384`, `nprobe=256`, `bits=4`)
- [x] 5.3 Export a stable result record that compares latency, recall, dimensions, rotation mode, and footprint for both paths

## 6. Validation

- [x] 6.1 Verify baseline random-rotation behavior is unchanged when padded-Hadamard mode is disabled
- [x] 6.2 Verify padded-Hadamard build/query reopen correctly and produce valid recall results
- [x] 6.3 Run the microbench gate and decide whether the padded-Hadamard candidate is worth full E2E comparison
- [x] 6.4 Run the full E2E comparison and record whether padded-Hadamard delivers a net serving benefit on MSMARCO
