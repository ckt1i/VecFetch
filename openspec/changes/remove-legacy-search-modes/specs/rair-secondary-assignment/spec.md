## ADDED Requirements

### Requirement: IVF builder SHALL support only single assignment as the formal build mode
The IVF builder SHALL create formal indexes with exactly one cluster assignment per vector. New formal builds SHALL use `assignment_mode = single` and `assignment_factor = 1`.

#### Scenario: New build uses single assignment
- **WHEN** a formal IVF-RaBitQ index is built
- **THEN** each vector SHALL be assigned to exactly one primary cluster
- **AND** the builder SHALL NOT generate secondary assignments

#### Scenario: Redundant assignment request is rejected
- **WHEN** a caller requests `assignment_factor != 1`, `redundant_top2_naive`, or `redundant_top2_rair`
- **THEN** the build path SHALL reject the request with a clear unsupported-mode error
- **AND** it SHALL NOT silently coerce the request into a different assignment mode

### Requirement: Formal query serving SHALL reject redundant assignment indexes
The formal query path SHALL require single-assignment index metadata. Redundant or RAIR-built indexes SHALL NOT be accepted as mainline serving inputs.

#### Scenario: Single-assignment index opens for serving
- **WHEN** an index metadata record has `assignment_mode = single` and `assignment_factor = 1`
- **THEN** the formal query path SHALL allow the index to be opened and searched

#### Scenario: Redundant index is not served silently
- **WHEN** an index metadata record indicates redundant top-2 or RAIR assignment
- **THEN** the formal query path SHALL fail clearly or mark the index as legacy unsupported
- **AND** it SHALL NOT execute the resident single-assignment hot path on that index

## REMOVED Requirements

### Requirement: IVF builder SHALL support RAIR-based secondary assignment
**Reason**: The project no longer treats redundant top-2 or RAIR assignment as a formal build or serving mode.
**Migration**: Rebuild indexes with single assignment.

### Requirement: RAIR configuration SHALL be explicit and observable
**Reason**: RAIR-specific build controls are removed from the formal configuration surface.
**Migration**: Benchmark and metadata outputs SHALL no longer rely on RAIR parameters.

### Requirement: RAIR mode SHALL remain compatible with existing redundant serving semantics
**Reason**: Redundant serving semantics are no longer part of the formal resident search path.
**Migration**: Use single-assignment indexes and resident single-assignment serving.
