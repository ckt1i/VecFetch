## Context

RecordGate's frozen paper protocol uses `non_safeout_candidate_budget=0`: every candidate that survives SafeOut remains eligible for exact-vector verification, and candidate count is controlled by ANN probing and evidence-based pruning rather than by a second global top-estimate cap. The implementation nevertheless still contains the older candidate-budget machinery and its speculative raw-vector prefetch path (`budgeted_prefetch_limit`, `SPEC_VEC_ONLY`, an evolving top-budget heap, pending speculative plans, in-flight/final-use tracking, and a query-local vector cache).

The scheduler also contains a newer SafeIn optional-I/O early-submit bypass. That mechanism can allow a bounded first group of optional payload requests to cross the mandatory-backlog guard, but controlled experiments did not produce a stable QPS gain and the mechanism remains disabled by default. Both paths cut across configuration, scheduler state, completion handling, statistics, CLI output, tests, and experiment scripts, so removing them requires an atomic cross-module cleanup.

The cleanup must not change index formats, RaBitQ classification, SafeOut, exact top-k semantics, mandatory vector-span coalescing, span payload reuse, bounded SafeIn tail/external prefetch, or final payload materialization.

## Goals / Non-Goals

**Goals:**

- Make the no-cap exact-verification path the only supported candidate flow after SafeOut.
- Remove all speculative raw-vector request state and behavior associated with the candidate budget.
- Remove the SafeIn optional early-submit mandatory-backlog bypass.
- Remove deleted controls and dedicated statistics from maintained benchmark surfaces and scripts.
- Preserve query correctness and the current mandatory/optional I/O priority contract.
- Validate cleanup with reused indexes and identical frozen parameters.

**Non-Goals:**

- Do not remove SafeOut, SafeIn classification, SafeIn tail extension, external payload prefetch, span payload reuse, or final materialization.
- Do not remove ordinary mandatory `VEC_ONLY` batching or amplification-bounded `VEC_SPAN` formation.
- Do not redesign candidate generation, change `nprobe`, add a replacement candidate cap, or introduce a new prefetch policy.
- Do not remove `SerialNoOverlap`; it remains a benchmark-only execution ablation.
- Do not change on-disk index, hot-record, sidecar, or payload formats.
- Do not claim a performance improvement from cleanup; the acceptance target is semantic equivalence and removal of inactive complexity.

## Decisions

### Decision 1: Remove the candidate-budget and speculative-vector path as one vertical slice

The implementation will remove `non_safeout_candidate_budget`, `budgeted_prefetch_limit`, `UseNonSafeOutCandidateBudget`, `AddBudgetedReadPlan`, `MaybeScheduleBudgetedPrefetch`, `MaterializeBudgetedReadPlans`, `TryUseSpeculativeVector`, speculative cache/finalization helpers, `SPEC_VEC_ONLY`, the budget heap, pending speculative queues, speculative offset sets/maps, and their dedicated counters.

After SafeOut filtering, candidates that require exact verification will enter the normal mandatory vector path. SafeIn may influence payload timing, but exact membership still requires the mandatory vector path.

Alternative considered: keep the candidate budget but remove only speculative prefetch. Rejected because the frozen protocol does not use the cap, the heap still changes candidate semantics, and retaining half of the feature would preserve most of the conceptual and testing complexity.

### Decision 2: Remove the SafeIn optional early-submit bypass, not optional I/O itself

The scheduler will remove `safein_optional_io_early_submit_max_requests` and the `allow_bounded_early_submit` branch. Optional payload I/O will continue to use the existing optional-I/O queue, capacity limits, mandatory-backlog guard, normal submit/refill policy, completion handling, and final drain.

Alternative considered: retain the branch as a hidden flag. Rejected because its experiments did not pass the end-to-end gate, a hidden branch still enlarges the scheduler state space, and stale scripts could continue to enable it.

### Decision 3: Treat removed CLI controls as errors during migration

Maintained help text, parsing, configuration output, and result schemas will stop exposing the three controls. For a transition period, benchmark entry points may retain a small rejection guard that exits with a clear message when an old command passes:

- `--non-safeout-candidate-budget`
- `--budgeted-prefetch-limit`
- `--safein-optional-io-early-submit-max-requests`

These guards must not populate runtime configuration or preserve the deleted behavior. This follows the project's prior hard-delete pattern and prevents a permissive argument parser from silently ignoring a stale paper script.

Alternative considered: silently ignore old flags. Rejected because it would make old budgeted runs appear to be valid no-cap runs.

### Decision 4: Remove dedicated observability while preserving invariant counters

Counters used only by the deleted paths will be removed from `SearchStats`, aggregation, JSON/CSV, and summaries. General counters required to verify behavior—recall, probed candidates, reranked candidates, mandatory vector requests/bytes, span requests/bytes, optional payload requests/bytes, final payload requests, submit/drain counts, and latency—will remain.

Historical result files are immutable evidence and need not be rewritten. Maintained parsers must tolerate historical files if they are intentionally used, but new output must not emit fixed-zero compatibility fields for deleted runtime behavior.

### Decision 5: Validate against the already-disabled configuration

The pre-cleanup reference must use `non_safeout_candidate_budget=0`, `budgeted_prefetch_limit=0`, and `safein_optional_io_early_submit_max_requests=0`. The post-cleanup run must reuse the same index, query set, ground truth, `topk`, `nprobe`, active/resident bits, routing configuration, span parameters, SafeIn parameters, cache protocol, warmup, query count, and repeat ordering.

Recall, total probed candidates, candidates reranked, and final result membership should be identical. Read counters unrelated to the deleted paths should also be identical; latency/QPS is guarded only against a material regression because the removal is not sold as an optimization.

## Risks / Trade-offs

- [Risk] A maintained runner still passes a removed flag and would otherwise silently change protocol. -> Mitigation: add explicit rejection guards and perform a repository-wide script audit.
- [Risk] Candidate-budget branches are interleaved with SafeIn plan construction and their removal changes which candidates receive mandatory vectors. -> Mitigation: add focused classification-to-read-plan tests for Uncertain and SafeIn candidates under the no-cap path.
- [Risk] Removing `SPEC_VEC_ONLY` changes pending-request counts, submit ordering, or final drain. -> Mitigation: assert no speculative request type remains and verify mandatory vector/request conservation in scheduler tests.
- [Risk] Removing optional early-submit accidentally disables all optional payload I/O. -> Mitigation: retain tests showing optional I/O submits after mandatory pressure clears and final payload materialization remains complete.
- [Risk] Historical analysis tools require deleted fields. -> Mitigation: update maintained parsers to treat them as optional historical fields without emitting them in new results.
- [Trade-off] Removing the cap eliminates a possible constrained-rerank research mode. -> Accepted because it is outside the frozen no-cap method; any future capped policy must return through a new change with explicit semantics and evidence.

## Migration Plan

1. Record a pre-cleanup no-cap reference command and metrics using existing indexes.
2. Add explicit CLI rejection tests for the three deleted flags, then remove their runtime parsing and output fields.
3. Remove candidate-budget and speculative-vector configuration, state, request type, helpers, completion handling, counters, and tests.
4. Collapse candidate plan construction onto the mandatory no-cap path and remove SerialNoOverlap's speculative-prefetch special case.
5. Remove SafeIn optional early-submit configuration, bypass logic, counters, and tests while retaining normal optional I/O.
6. Update maintained scripts, parsers, help text, and paper experiment controls.
7. Build, run focused scheduler/benchmark tests, and run same-index no-cap regression points.
8. Rollback by reverting this change as one atomic unit if semantic counters diverge; do not partially restore only the cache or request type.

## Open Questions

- Which maintained paper runner is the canonical source for the final no-cap command, so the script audit can use it as the protocol anchor?
- Should explicit deleted-flag rejection remain for one release cycle or indefinitely as a low-cost protection against historical scripts?

