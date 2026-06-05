## 1. Baseline Audit

- [x] 1.1 Run `rg` audits for HNSW coarse routing, assignment mode/factor, RAIR, secondary assignments, pad-to-pow2, blocked Hadamard, and FHT-Kac references.
- [x] 1.2 Record the current mainline COCO100k and MS MARCO benchmark commands from `test_configs.txt` or recent validation notes.
- [x] 1.3 Confirm `remove-window-cluster-read` behavior is present: resident full-preload is the only formal cluster-side query read path.
- [x] 1.4 Build the current tree once before cleanup to separate pre-existing build failures from this change.

## 2. Remove HNSW Coarse Routing

- [x] 2.1 Remove `SearchConfig` HNSW coarse routing fields and HNSW stats fields from query context.
- [x] 2.2 Remove `IvfIndex::SetHnswCoarseRouting`, `PrepareHnswCoarseRouting`, `EnsureHnswCoarseGraph`, `FindNearestClustersHnsw`, and HNSW runtime state.
- [x] 2.3 Remove direct project includes and usage of `faiss::IndexHNSWFlat` for centroid routing.
- [x] 2.4 Update `FindNearestClusters()` so formal routing only considers exact and supported two-level coarse routing.
- [x] 2.5 Remove HNSW CLI arguments and HNSW result fields from `bench_e2e` JSON/CSV/per-query output.
- [x] 2.6 Update or delete HNSW-specific unit tests in `tests/index/ivf_index_test.cpp`.

## 3. Consolidate To Single Assignment

- [x] 3.1 Remove formal `assignment_factor`, `assignment_mode`, `rair_lambda`, `rair_strict_second_choice`, and `save_secondary_assignments_path` builder configuration from public build/benchmark inputs.
- [x] 3.2 Remove `DeriveSecondaryAssignments()` and secondary assignment materialization from the build pipeline, or reduce it to an impossible legacy path with no formal caller.
- [x] 3.3 Ensure `.clu` cluster membership construction uses only primary `assignments_` and never duplicates vectors into secondary clusters.
- [x] 3.4 Ensure new metadata always records single assignment semantics: `assignment_mode = SINGLE` and `assignment_factor = 1`.
- [x] 3.5 Add open/search validation that rejects redundant or RAIR metadata as legacy unsupported before resident single-assignment serving runs.
- [x] 3.6 Remove assignment-mode, RAIR, and secondary-assignment fields from `bench_e2e` configuration output, CSV rows, logs, and output directory naming.
- [x] 3.7 Update builder/query tests that currently construct redundant top-2 or RAIR indexes to expect unsupported mode or remove them if they only validate deleted behavior.

## 4. Consolidate Non-Power-Of-Two Rotation To FHT-Kac

- [x] 4.1 Remove `--pad-to-pow2` from `bench_vector_search` and delete its manual base/query/centroid padding helper usage from the formal path.
- [x] 4.2 Remove `--pad-to-pow2`, `--blocked-hadamard-permuted`, and user-facing `--fht-kac-rotator` opt-in from `bench_e2e`; non-power-of-two builds must choose FHT-Kac automatically.
- [x] 4.3 Update `IvfBuilder` dimension/rotation selection so power-of-two dimensions use Hadamard and non-power-of-two dimensions use FHT-Kac without requiring a user flag.
- [x] 4.4 Remove formal builder config fields for `pad_non_power_of_two_to_pow2` and `use_blocked_hadamard_permuted`.
- [x] 4.5 Remove blocked/padded Hadamard rows from rotation benchmark scripts or replace those scripts with FHT-Kac-only validation helpers.
- [x] 4.6 Add build/open tests proving a 768-dimensional build records `rotation_mode = "fht_kac_rotator"` and does not produce `hadamard_padded`, `blocked_hadamard_permuted`, or `random_matrix`.
- [x] 4.7 Decide whether blocked Hadamard implementation remains as legacy deserialization/error-reporting code or is fully deleted; update tests to match that decision.

## 5. Query Pipeline And Metadata Guards

- [x] 5.1 Add or update query-path guards so formal resident search requires single-assignment metadata.
- [x] 5.2 Add or update query-path guards so formal resident search rejects legacy `hadamard_padded` and `blocked_hadamard_permuted` rotation modes clearly.
- [x] 5.3 Ensure non-power-of-two FHT-Kac query path still performs query-once rotation and uses pre-rotated centroids.
- [x] 5.4 Verify exact and two-level coarse routing stats remain correct after removing HNSW routing mode.
- [x] 5.5 Keep FlatBuffers schema fields as legacy compatibility unless implementation proves schema edits are unavoidable; do not require index format migration.

## 6. Benchmark And Script Cleanup

- [x] 6.1 Update `test_configs.txt` and any maintained benchmark scripts to remove deleted parameters.
- [x] 6.2 Remove output parsing or aggregation assumptions for HNSW, RAIR, secondary assignment, padded Hadamard, and blocked Hadamard fields.
- [x] 6.3 Ensure benchmark outputs still include dataset, index path, `nlist`, `nprobe`, `topk`, `bits`, metric, rotation mode, routing mode, recall, latency, preload, and SafeIn/SafeOut/Uncertain metrics.
- [x] 6.4 Update documentation or comments in benchmark usage strings so removed knobs are no longer advertised.

## 7. Tests

- [x] 7.1 Run targeted index tests after HNSW and assignment cleanup.
- [x] 7.2 Run targeted builder tests after single-assignment and FHT-Kac consolidation.
- [x] 7.3 Run targeted query scheduler tests for resident single-assignment serving.
- [x] 7.4 Run targeted RaBitQ rotation and estimator tests after removing blocked/padded formal paths.
- [x] 7.5 Run the benchmark binaries' compile targets for `bench_e2e` and `bench_vector_search`.

## 8. Validation Benchmarks

- [x] 8.1 Run COCO100k `bench_e2e` with the current mainline test_config or documented equivalent command.
- [x] 8.2 Run MS MARCO `bench_e2e` with the current mainline FHT-Kac test_config or documented equivalent command.
- [x] 8.3 Run COCO100k `bench_vector_search` vector-only validation without `--pad-to-pow2`.
- [x] 8.4 Record speed, recall, coarse routing mode, rotation mode, SafeIn/SafeOut/Uncertain, preload, and query breakdown results.
- [x] 8.5 Compare results against the most recent pre-cleanup mainline run and note any non-comparable parameter changes.
