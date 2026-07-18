# Auto Review Round 1

- Score: 4.8/10
- Verdict: REVISE
- Decision: M0 implementation may begin; perf and four-way experiments remain
  blocked until correctness gates pass.

## Critical findings

1. Current code contains only duplicated async/serial GV implementations.
2. The previous plan incorrectly called exact-vector existing and excluded the
   O(n log n) solver now required by the research goal.
3. SafeIn credit needs a closed storage/view contract and internal-member-only
   accounting.
4. Exact arithmetic, deterministic ties, an O(n^2) oracle, CLI provenance,
   planner telemetry, and async/serial parity tests are mandatory.
5. The secondary two-dataset rule must be preregistered over three datasets and
   retain correctness, p99, bytes, request, and original-core hard gates.

## Minimum fix before Round 2

- Add one shared pure planner with GV/SV/GE/SE.
- Implement rational single-cap O(n log n) exact planning.
- Add randomized oracle differential tests and storage-credit tests.
- Integrate the same planner into async and serial paths.
- Add CLI, JSON provenance, planner telemetry, and fail-fast validation.

