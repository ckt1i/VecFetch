## Why

Runtime SafeIn/SafeOut epsilon percentile calibration is currently too expensive for iterative benchmark sweeps because `--epsilon-samples` is interpreted as per-cluster query count and each sampled query is compared with all members in the cluster. This makes SafeOut percentile experiments slow enough to block routine validation of CPU-side query optimizations.

## What Changes

- Add a fast global-pair epsilon calibration mode where `--epsilon-samples K` means exactly K sampled query-target pairs.
- Preserve the existing per-cluster calibration mode and default behavior for compatibility.
- Expose the selected calibration mode in benchmark config/results so runs are reproducible.
- Validate the fast mode against the existing calibration path and real GT query benchmarks.

## Capabilities

### New Capabilities
- `epsilon-calibration-sampling`: Defines selectable epsilon calibration sampling modes for benchmark-time SafeIn/SafeOut epsilon estimation.

### Modified Capabilities
- None.

## Impact

- Affects benchmark calibration code used by `bench_e2e` and `bench_vector_search`.
- Adds CLI/config/reporting surface for epsilon sampling mode.
- Does not change index format, query runtime semantics, default epsilon behavior, or existing result fields.
