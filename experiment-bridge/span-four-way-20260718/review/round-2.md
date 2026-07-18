# Auto Review Round 2

- Score: 7.6/10
- Verdict: ALMOST
- Decision: the mathematical core is accepted, but perf remains blocked until
  the three P0 integration gaps are closed.

## Confirmed

- The exact solver implements the correct one-dimensional endpoint-dominance
  transform and Fenwick suffix minimum in `O(n log n)`.
- SafeIn credit is `prefix_credit[end - 1] - prefix_credit[begin]`, so only
  internal members contribute and the endpoint is excluded.
- The exact objective is lexicographic `(requests, physical bytes)` with a
  deterministic earliest-predecessor tie break.
- The 800-case GE/SE differential test compares complete partitions against an
  independent quadratic oracle.
- Async and serial paths now call the same production planner helper.

## P0 fixes required before perf

1. Preserve or explicitly reject the legacy float amplification CLI; it must
   not be silently ignored when the rational alpha is absent.
2. Planner/arithmetic/partition failure must be fail-fast for experiments, not
   silently fall back to singleton reads.
3. Add issued-I/O contract tests and credited-completion tests, including zero
   credit for cold, missing metadata, sidecar, and insufficient coverage.

SafeIn-aware modes must also fail fast when reuse, inline layout, or resident
metadata is missing. The legacy endpoint-tail extension is outside the frozen
model and cannot be combined with the rational planner without an explicit
post-partition contract.
