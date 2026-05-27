## Context

`bench_e2e` and `bench_vector_search` currently call `CalibrateSplitEpsilon()` when SafeIn/SafeOut epsilon percentiles are requested. The existing function treats `--epsilon-samples` as a per-cluster query budget and compares each sampled query against every other member in the same cluster. For MSMARCO-scale indexes this turns a simple SafeOut percentile sweep into a long offline calibration job.

## Goals / Non-Goals

**Goals:**
- Add a fast calibration mode where `--epsilon-samples K` performs exactly K global query-target pair error samples.
- Keep the existing per-cluster calibration mode as the default.
- Report the selected mode, sample count, and realized error count in benchmark JSON.
- Validate that `K=1000/5000/10000` gives useful SafeOut epsilon estimates under real GT recall benchmarks.

**Non-Goals:**
- Do not change query-time SafeOut/SafeIn semantics.
- Do not change index format or persisted metadata.
- Do not replace final high-confidence calibration for report-quality runs.
- Do not couple this change to Stage2 kernel or uncertain-candidate reduction work.

## Decisions

- Add `--epsilon-sampling-mode legacy_per_cluster|global_pair`.
  - `legacy_per_cluster` preserves current behavior and remains default.
  - `global_pair` reinterprets `--epsilon-samples` as total sampled pairs, not per-cluster query count.

- Implement global sampling by selecting query rows proportional to real data frequency.
  - Build or reuse a row-to-cluster/local-index mapping from `cluster_members`.
  - For each sample, select a global row uniformly, find its cluster, then select one different target row from the same cluster.
  - This avoids over-weighting tiny clusters, which would happen if clusters were sampled uniformly.

- Keep one pair equal to one error contribution.
  - Each accepted sample computes true L2, estimated RabitQ distance, normalized error, then contributes one value to `UpperPercentile`.
  - If a sample is invalid because the cluster has fewer than two members, denominator is non-positive, or distance is outside `[0.1*d_k, 10*d_k]`, retry until either K valid errors are collected or a bounded retry limit is reached.

- Share the mode through the existing calibration call sites.
  - Extend the calibration API with a sampling mode enum and optional output stats.
  - Wire both `bench_e2e` and `bench_vector_search` to the same implementation.

## Risks / Trade-offs

- Tail under-estimation with small K -> validate recall and false SafeOut with real GT; recommend K=1000 for quick sweeps and K>=10000 for stronger baselines.
- Random seed variance -> keep existing seed behavior and run multi-seed validation for K=1000/5000/10000.
- Different weighting from legacy mode -> use global-row weighted sampling so fast mode better matches query distribution.
- Retry loops on sparse/filtered data -> cap attempts and report realized valid error count so failed calibration is visible.
