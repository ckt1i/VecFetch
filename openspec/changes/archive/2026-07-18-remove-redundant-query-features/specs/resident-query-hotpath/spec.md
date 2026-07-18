## ADDED Requirements

### Requirement: Resident hot path SHALL use the frozen fixed-bit serving configuration
The resident hot path SHALL treat fixed active ex-bits as the supported Stage2 execution mode for the frozen implementation. Runtime selection of active ex-bits MAY remain available only when it maps to a fixed query configuration, not as progressive per-query pruning.

#### Scenario: Fixed active bits are used for resident query
- **WHEN** resident full-preload serving runs with an active ex-bits setting
- **THEN** the hot path MUST evaluate Stage2 using that fixed active-bit setting
- **AND** it MUST NOT split the same query into progressive pruning rounds

#### Scenario: Selective resident preload remains supported
- **WHEN** resident serving loads fewer active ex-bits than the physical index stores
- **THEN** the hot path MUST support querying the resident subset selected at preload time
- **AND** the cleanup MUST NOT require loading all stored ex-bits

### Requirement: Resident hot path SHALL not retain removed feature statistics as formal outputs
Resident query statistics SHALL remove formal output fields that only describe abandoned features, including progressive pruning rounds, progressive SafeOut lanes, Stage1 envelope skip counts, address-sort-only fields, and budgeted-early-submit-only counters.

#### Scenario: Removed statistics are not exported
- **WHEN** resident benchmark output is written after cleanup
- **THEN** the output MUST NOT include formal JSON fields dedicated only to removed progressive pruning, Stage1 envelope skip, address sorting, or budgeted early submit
- **AND** official timing fields for prepare, Stage1, Stage2, classify, submit, coarse select, and end-to-end latency MUST remain available

### Requirement: Resident hot path SHALL keep stable submit and rerank behavior
The cleanup SHALL preserve resident vector-only submit behavior, tail flush, final drain, rerank count reporting, and result ordering for the supported frozen path.

#### Scenario: Tail flush and final drain remain correct
- **WHEN** a resident query finishes probing and still has pending vector-only candidates
- **THEN** tail flush and final drain MUST emit and consume all eligible candidates exactly once
- **AND** final top-k results MUST remain equivalent to the pre-cleanup fixed path
