## ADDED Requirements

### Requirement: E2E benchmark SHALL reject deleted budget and early-submit controls
Maintained benchmark entry points MUST NOT expose candidate-budget selection, budgeted speculative-vector prefetch, or SafeIn optional early-submit as supported controls. Commands that pass a deleted flag MUST fail clearly rather than silently running a different protocol.

#### Scenario: Candidate-budget flag is rejected
- **WHEN** a maintained benchmark command passes `--non-safeout-candidate-budget`
- **THEN** the benchmark MUST exit with a clear unsupported-option error
- **AND** it MUST NOT run a capped candidate-selection protocol

#### Scenario: Speculative prefetch flag is rejected
- **WHEN** a maintained benchmark command passes `--budgeted-prefetch-limit`
- **THEN** the benchmark MUST exit with a clear unsupported-option error
- **AND** it MUST NOT create speculative raw-vector requests

#### Scenario: SafeIn optional early-submit flag is rejected
- **WHEN** a maintained benchmark command passes `--safein-optional-io-early-submit-max-requests`
- **THEN** the benchmark MUST exit with a clear unsupported-option error
- **AND** it MUST not enable a mandatory-backlog bypass

### Requirement: E2E benchmark output SHALL omit deleted-path fields
New benchmark output MUST NOT emit configuration, counters, or derived metrics dedicated only to candidate budgets, speculative-vector prefetch, or SafeIn optional early-submit. It SHALL retain general correctness, I/O, span, payload, memory, and latency fields needed to evaluate the supported pipeline.

#### Scenario: New result schema contains no deleted-path metrics
- **WHEN** a benchmark writes configuration and result metadata after this change
- **THEN** the output MUST omit deleted budget, speculative-prefetch, and optional-early-submit fields
- **AND** it MUST retain recall, probed candidates, reranked candidates, supported read counters, and latency metrics

### Requirement: Cleanup SHALL be validated with a same-index no-cap regression
The benchmark workflow MUST compare a pre-cleanup run with all removed features disabled against a post-cleanup run using the same index, query/GT assets, search parameters, storage layout, cache protocol, warmup, and repeat ordering.

#### Scenario: No-cap cleanup preserves search semantics
- **WHEN** pre-cleanup and post-cleanup runs use the frozen no-cap operating point
- **THEN** recall, total probed candidates, candidates reranked, and final result semantics MUST be identical
- **AND** any difference in supported read counters or latency MUST be reported and explained before the change is accepted

