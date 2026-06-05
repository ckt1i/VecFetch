## ADDED Requirements

### Requirement: IVF-RaBitQ storage SHALL persist official direct compact layout identity
The IVF-RaBitQ baseline storage metadata SHALL persist the official ExData compact layout identity for optimized `total_bits=4, ex_bits=3` indexes.

#### Scenario: Optimized index metadata records layout identity
- **WHEN** an optimized official `1+3` IVF-RaBitQ index is built
- **THEN** its metadata MUST include a stable layout key for `2-bit + 1-bit`, `1-bit + 1-bit + 1-bit`, or selected direct layout
- **AND** the metadata MUST continue to report `rabitq_estimator_mode`, `rabitq_total_bits`, and `rabitq_ex_bits`

#### Scenario: Experimental layouts write separate index directories
- **WHEN** the builder creates both optimized candidate layouts for the comparison run
- **THEN** the two indexes MUST be written to distinct paths
- **AND** the paths or metadata MUST make it clear which layout each index uses

### Requirement: Selected official 3-bit layout SHALL be reproducible from storage metadata
Once the benchmark comparison selects a winning layout, the selected layout SHALL be reproducible from stored metadata and index path conventions.

#### Scenario: Selected layout can be reopened without CLI ambiguity
- **WHEN** a selected optimized official `1+3` index is reopened for query
- **THEN** the query binary MUST infer the selected direct compact layout from the index metadata
- **AND** it MUST NOT require the user to manually specify a conflicting layout flag

#### Scenario: Non-selected layout is isolated from main results
- **WHEN** the non-selected layout index remains on disk after comparison
- **THEN** it MUST be labeled as experimental or rejected in metadata or result summaries
- **AND** it MUST NOT be mistaken for the selected official baseline index
