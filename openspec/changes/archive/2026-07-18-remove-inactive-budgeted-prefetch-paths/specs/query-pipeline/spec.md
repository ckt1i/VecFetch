## ADDED Requirements

### Requirement: Query pipeline SHALL use an uncapped post-SafeOut verification path
The query pipeline SHALL treat every candidate that survives SafeOut and requires exact verification as eligible for the normal mandatory raw-vector path. It MUST NOT apply a second `non_safeout_candidate_budget` selection heap or drop a surviving candidate because of that deleted budget.

#### Scenario: Uncertain candidate enters mandatory vector verification
- **WHEN** a candidate is not removed by SafeOut and requires exact-vector verification
- **THEN** the scheduler MUST enqueue the candidate through the mandatory vector-read path
- **AND** it MUST NOT route the candidate through a global top-estimate candidate-budget heap

#### Scenario: SafeIn does not replace exact verification
- **WHEN** a SafeIn candidate still requires exact membership confirmation
- **THEN** its raw vector MUST remain eligible for the mandatory verification path
- **AND** any payload timing policy MUST remain separate from exact-vector correctness

### Requirement: Query pipeline SHALL not issue speculative raw-vector requests from a candidate budget
The query pipeline MUST NOT expose or emit a `SPEC_VEC_ONLY` request type, maintain pending speculative vector plans, or cache raw vectors on behalf of a deleted candidate-budget prefetch policy. Vector-span coalescing SHALL continue to operate on mandatory vector reads according to its existing admission rules.

#### Scenario: Mandatory vector submission contains no speculative request type
- **WHEN** the scheduler emits raw-vector I/O for candidates surviving SafeOut
- **THEN** every emitted raw-vector request MUST use a supported mandatory or span request type
- **AND** no request MUST depend on `budgeted_prefetch_limit` or speculative final-use tracking

#### Scenario: Query finalization requires no speculative cache cleanup
- **WHEN** a query finalizes its exact top-k and payload results
- **THEN** finalization MUST complete without a speculative-vector cache, speculative-offset set, or speculative wasted-prefetch accounting

### Requirement: SerialNoOverlap SHALL remain valid without speculative-prefetch special handling
`SerialNoOverlap` SHALL remain a benchmark-only execution mode, but its correctness MUST NOT depend on forcing a deleted speculative-prefetch limit to zero or rejecting pending `SPEC_VEC_ONLY` plans.

#### Scenario: SerialNoOverlap runs the unified no-cap candidate flow
- **WHEN** a query runs in `SerialNoOverlap` mode
- **THEN** it MUST use the same uncapped post-SafeOut candidate semantics as the default execution mode
- **AND** it MUST complete without speculative-vector configuration or pending speculative plans

