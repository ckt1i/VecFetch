## 1. Scope Lock and Pre-Cleanup Reference

- [x] 1.1 Record the canonical frozen no-cap command, index paths, query/GT assets, cache/warmup protocol, repeat order, binary metadata, and output location for representative validation points.
- [x] 1.2 Run or identify a pre-cleanup reference with `non_safeout_candidate_budget=0`, `budgeted_prefetch_limit=0`, and `safein_optional_io_early_submit_max_requests=0`, and preserve recall, probed, reranked, read, span, payload, and latency metrics.
- [x] 1.3 Use code-review-graph first, followed by targeted text search, to inventory every config field, enum value, helper, queue, cache, counter, CLI flag, JSON/CSV field, test, maintained script, and parser associated with the three deleted controls.
- [x] 1.4 Record explicit preservation boundaries for SafeOut, mandatory `VEC_ONLY`, `VEC_SPAN`, span payload reuse, SafeIn tail/external prefetch, normal optional-I/O admission, final payload materialization, and `SerialNoOverlap`.

## 2. Remove Candidate-Budget Configuration and Selection

- [x] 2.1 Remove `non_safeout_candidate_budget` from query configuration, benchmark parsing/help/config output, and maintained experiment controls.
- [x] 2.2 Remove candidate-budget statistics and aggregation, including seen, selected, and dropped counters that have no supported no-cap meaning.
- [x] 2.3 Remove the budget heap, `BudgetedReadPlan`, `UseNonSafeOutCandidateBudget`, `AddBudgetedReadPlan`, `MaterializeBudgetedReadPlans`, and all related reset/finalization state.
- [x] 2.4 Collapse Uncertain and SafeIn exact-verification plan construction onto the supported uncapped mandatory vector path while preserving candidate deduplication and final exact top-k semantics.
- [x] 2.5 Add or update focused tests showing that every post-SafeOut candidate requiring exact verification remains eligible for mandatory vector reading without a global candidate cap.

## 3. Remove Speculative Raw-Vector Prefetch

- [x] 3.1 Remove `budgeted_prefetch_limit` from query configuration, benchmark parsing/help/config output, and maintained experiment controls.
- [x] 3.2 Remove `PendingIO::Type::SPEC_VEC_ONLY`, the pending speculative plan queue/head, speculative offset sets, final-use set, speculative vector cache, and associated buffer ownership.
- [x] 3.3 Remove speculative scheduling, cache lookup, in-flight final-use marking, completion dispatch, cache cleanup, wasted-prefetch finalization, and all call sites.
- [x] 3.4 Remove dedicated speculative-prefetch counters and their benchmark aggregation and JSON/CSV fields while preserving general mandatory-vector and span counters.
- [x] 3.5 Remove `SerialNoOverlap` checks that only disabled or rejected speculative prefetch, and verify that the mode still runs through the unified no-cap candidate flow.
- [x] 3.6 Update scheduler tests to verify that no speculative request type or cache state remains and that mandatory vector/request conservation still holds through submit, completion, tail flush, and final drain.

## 4. Remove SafeIn Optional Early-Submit

- [x] 4.1 Remove `safein_optional_io_early_submit_max_requests` from query configuration, benchmark parsing/help/config output, and maintained experiment controls.
- [x] 4.2 Remove `allow_bounded_early_submit`, first-batch mandatory-guard bypass logic, and early-submit request/call accounting from the optional-I/O scheduler.
- [x] 4.3 Remove dedicated optional early-submit counters, aggregation, JSON/CSV fields, logs, and tests.
- [x] 4.4 Add or update tests showing that mandatory backlog blocks optional SafeIn payload submission under the normal policy and that optional I/O resumes after normal admission conditions are satisfied.
- [x] 4.5 Verify that removing early-submit does not remove SafeIn tail extension, external payload prefetch, optional-I/O isolation, or final missing-payload fetch.

## 5. CLI, Script, and Result-Schema Migration

- [x] 5.1 Add explicit unsupported-option rejection for `--non-safeout-candidate-budget`, `--budgeted-prefetch-limit`, and `--safein-optional-io-early-submit-max-requests` without retaining runtime behavior.
- [x] 5.2 Add benchmark argument tests proving that each deleted flag exits non-zero with a clear migration message.
- [x] 5.3 Search maintained runners, paper experiment controls, shell scripts, analysis tools, and documentation; remove deleted flags from active commands and label historical result commands as non-runnable legacy evidence where retained.
- [x] 5.4 Update maintained parsers so new results do not require or emit deleted fields while intentionally loaded historical files may still contain them.
- [x] 5.5 Perform a literal residual audit for `non_safeout_candidate_budget`, `budgeted_prefetch_limit`, `SPEC_VEC_ONLY`, `budgeted_prefetch_`, `safein_optional_io_early_submit`, and speculative-cache symbols, allowing only OpenSpec/history and explicit deleted-flag rejection strings.

## 6. Build and Focused Correctness Validation

- [x] 6.1 Build affected benchmark, query, and test targets after each removal group so compilation exposes stale cross-module dependencies.
- [x] 6.2 Run focused scheduler tests covering SafeOut, Uncertain and SafeIn plan creation, mandatory vector batching, vector spans, optional payload scheduling, final payload completeness, and `SerialNoOverlap`.
- [x] 6.3 Run benchmark argument/schema tests and a fast smoke query on an existing index to catch CLI, JSON/CSV, and result-parser regressions.
- [x] 6.4 Verify recall, final result ordering, probed candidates, reranked candidates, and supported read accounting remain internally consistent.

## 7. Same-Index No-Cap Regression

- [x] 7.1 Run the post-cleanup binary on the pre-registered same-index no-cap validation points with identical queries, GT, parameters, span/SafeIn configuration, cache protocol, warmup, repeats, and ordering.
- [x] 7.2 Require identical recall, total probed candidates, candidates reranked, and final result semantics between pre-cleanup and post-cleanup runs.
- [x] 7.3 Compare mandatory vector requests/bytes, span requests/bytes, payload requests/bytes, optional-I/O counts, final fetches, latency, and QPS; explain any difference before accepting the change.
- [x] 7.4 Treat a confirmed semantic difference or material performance regression as a failure and revert the cleanup atomically rather than restoring only part of the deleted speculative state machine.
- [x] 7.5 Write a final validation report with commands, artifact paths, before/after metrics, residual-symbol audit, and pass/fail conclusion.

## 8. OpenSpec and Paper-Protocol Closure

- [x] 8.1 Update current method documentation so `VEC_ONLY` and `VEC_SPAN` are described only as mandatory exact-verification reads under the no-cap protocol, without an active speculative-vector branch.
- [x] 8.2 Update SafeIn documentation so optional payload I/O uses normal admission and SafeIn is not described as having an early-submit bypass.
- [x] 8.3 Verify that paper-facing experiment plans no longer list candidate-budget or optional early-submit sweeps as current method dimensions.
- [x] 8.4 Run OpenSpec status/validation and confirm all specified removal, preservation, migration, and regression requirements are covered by completed tasks and evidence.
