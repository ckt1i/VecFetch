# Deleted-path symbol inventory

The code-review graph was consulted first (`get_minimal_context` and file summaries for `search_context.h`, `overlap_scheduler.h/.cpp`, `bench_online_query.cpp`, and `overlap_scheduler_test.cpp`), followed by a literal repository search.

## Query configuration and statistics

- `SearchConfig`: `non_safeout_candidate_budget`, `budgeted_prefetch_limit`, `safein_optional_io_early_submit_max_requests`.
- `SearchStats`: `candidate_budget_*`, `budgeted_prefetch_*`, `safein_optional_io_early_submit_*`.

## Scheduler state and helpers

- Request/state: `SPEC_VEC_ONLY`, `SpeculativeVector`, `BudgetedReadPlan`, `pending_spec_vec_plans_`, `budgeted_read_plan_heap_`, `pending_spec_vec_head_`, speculative offset sets and cache.
- Helpers: `UseNonSafeOutCandidateBudget`, `AddBudgetedReadPlan`, `MaybeScheduleBudgetedPrefetch`, `TryUseSpeculativeVector`, `CacheSpeculativeVector`, `FinalizeBudgetedPrefetchStats`, `MaterializeBudgetedReadPlans`.
- Optional bypass: `allow_bounded_early_submit` and its first-batch cap/accounting.
- Call-site families: constructor reservation, per-query reset/finalization, pending-count accounting, vector emit, completion dispatch, candidate-plan builders, serial-mode guard.

## Benchmark surface

- `bench_online_query`: metric aggregation, help, parsing, SerialNoOverlap force-zero logic, trace/oracle guards, configuration JSON, pipeline-stat JSON.
- `bench_e2e`: argument parsing/validation, config propagation, log summary, JSON output, candidate-budget statistics.
- Maintained runner: `benchmarks/scripts/run_no_combine_ablation.py` still passed a nonzero candidate budget.
- Frozen external formal runner explicitly passed zeros and must be migrated before reuse.

## Tests

- Fixture/config helpers accepted a candidate budget and optional early-submit cap.
- Optional-I/O tests asserted early-submit request counts.
- SerialNoOverlap test explicitly set a speculative prefetch limit and asserted zero scheduled requests.

## Allowed historical residues after implementation

- This OpenSpec change and archived/completed OpenSpec artifacts describing the removed behavior.
- Immutable historical result JSON and result-analysis documents.
- Explicit deleted-flag rejection strings in maintained benchmark entry points.

