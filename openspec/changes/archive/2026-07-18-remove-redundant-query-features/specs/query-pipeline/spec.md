## ADDED Requirements

### Requirement: Query pipeline SHALL remove abandoned experimental pruning controls
The query pipeline SHALL NOT expose Stage2 progressive active-bits pruning, Stage2 per-bit pruning, or Stage1 block-level skip envelope as supported query controls in the frozen serving path. The fixed active ex-bits path and standard Stage1/Stage2 SafeOut semantics MUST remain the supported path.

#### Scenario: Progressive pruning flags are unavailable
- **WHEN** a benchmark or serving command is configured after this cleanup
- **THEN** Stage2 progressive active-bits and per-bit pruning flags MUST NOT be accepted as supported runtime controls
- **AND** the query MUST use the fixed active ex-bits path

#### Scenario: Stage1 envelope skip is unavailable
- **WHEN** a resident query probes cluster blocks after this cleanup
- **THEN** Stage1 block-level skip envelope MUST NOT participate in SafeOut decisions
- **AND** standard Stage1 mask and Stage2 refinement semantics MUST remain unchanged

### Requirement: Query pipeline SHALL remove abandoned submit scheduling variants
The query pipeline SHALL NOT expose vector-read address sorting or budgeted early submit as supported scheduling variants in the frozen serving path. Existing fixed submit, flush, drain, SafeIn, SafeOut, and prefetch behavior MUST remain available.

#### Scenario: Address sorting is not used for vector-read submit
- **WHEN** the scheduler emits vector-only data reads in the frozen serving path
- **THEN** it MUST NOT reorder candidates through the abandoned vector-read address sorting option
- **AND** candidate correctness and final result ordering MUST remain equivalent to the pre-cleanup fixed path

#### Scenario: Budgeted early submit is not used during probing
- **WHEN** the scheduler processes probed candidates before final drain
- **THEN** it MUST NOT emit candidates through the abandoned budgeted early-submit queue
- **AND** normal submit batching, tail flush, and final drain MUST preserve all eligible candidates

### Requirement: Query pipeline SHALL preserve frozen routing and pipeline behavior
The cleanup SHALL preserve the frozen query behavior: fixed active ex-bits, selective resident preload when configured, compact batched preload when configured, two-level coarse routing when configured, and SafeIn/SafeOut plus prefetch pipeline semantics.

#### Scenario: Frozen query controls remain available
- **WHEN** a benchmark enables resident serving with fixed active ex-bits and two-level coarse routing
- **THEN** the query pipeline MUST accept and apply those supported controls
- **AND** the cleanup MUST NOT force fallback to a non-resident or non-two-level path

#### Scenario: SafeIn and SafeOut semantics are unchanged
- **WHEN** a query is executed with the same index, query vectors, and supported frozen parameters before and after cleanup
- **THEN** the SafeIn/SafeOut candidate semantics MUST remain equivalent
- **AND** final recall MUST stay within the cleanup validation tolerance
