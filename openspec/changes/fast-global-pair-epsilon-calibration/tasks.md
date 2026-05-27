## 1. Calibration API and CLI

- [x] 1.1 Add an epsilon sampling mode enum with `legacy_per_cluster` and `global_pair`.
- [x] 1.2 Extend `CalibrateSplitEpsilon` inputs to accept the sampling mode and optional output stats.
- [x] 1.3 Add `--epsilon-sampling-mode` parsing to `bench_e2e` and `bench_vector_search`, defaulting to `legacy_per_cluster`.
- [x] 1.4 Include sampling mode, requested sample count, realized valid error count, percentile, and runtime epsilon in benchmark JSON/config output.

## 2. Global Pair Calibration

- [x] 2.1 Build or reuse a row-to-cluster/local-index mapping from `cluster_members`.
- [x] 2.2 Implement `global_pair` sampling as global-row weighted query selection plus one random in-cluster target row.
- [x] 2.3 Ensure each valid pair produces exactly one normalized error contribution.
- [x] 2.4 Add bounded retry handling for singleton clusters, invalid denominators, and distances outside `[0.1*d_k, 10*d_k]`.
- [x] 2.5 Preserve the current per-cluster algorithm unchanged when mode is `legacy_per_cluster`.

## 3. Tests

- [x] 3.1 Add unit coverage for global pair sample-count semantics: requested K yields at most K valid errors and never scales by cluster count.
- [x] 3.2 Add deterministic seed tests showing repeated runs produce the same sampled epsilon on fixed toy data.
- [x] 3.3 Add compatibility coverage showing default mode calls the legacy per-cluster behavior.
- [x] 3.4 Add CLI smoke coverage for both benchmark entrypoints accepting `--epsilon-sampling-mode global_pair`.

## 4. Validation

- [x] 4.1 Run a fast calibration sanity check with `--epsilon-sampling-mode global_pair --epsilon-samples 1000`.
- [x] 4.2 Run MSMARCO `fht_kac_rotator` real-GT benchmarks for K=`1000`, `5000`, and `10000` with SafeOut p95.
- [x] 4.3 Compare against the current default SafeOut epsilon baseline on recall@10, runtime SafeOut epsilon, `s2_uncertain`, `vec_only_reads`, `probe_submit_ms`, and `avg_query_time_ms`.
- [x] 4.4 Run at least three seeds for K=`1000` to estimate variance of runtime SafeOut epsilon and recall@10.
- [x] 4.5 Document recommended K values for quick sweeps and stronger baselines based on observed variance/cost.
