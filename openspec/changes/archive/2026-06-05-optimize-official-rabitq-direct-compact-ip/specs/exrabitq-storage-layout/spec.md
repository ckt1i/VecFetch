## ADDED Requirements

### Requirement: Official 3-bit optimized layouts SHALL be explicitly versioned
The ExRaBitQ cluster store SHALL explicitly distinguish generic v13 official ExData bitstream storage from optimized official `ex_bits=3` direct-IP storage layouts. The distinction MUST be represented by a durable version, layout id, or metadata key that is validated by the reader before query execution.

#### Scenario: Reader distinguishes generic and optimized official layouts
- **WHEN** a reader opens an official `total_bits=4, ex_bits=3` index
- **THEN** it MUST determine whether Region2 uses generic packed ExData, `2-bit + 1-bit` direct layout, or `1-bit + 1-bit + 1-bit` direct layout from explicit metadata
- **AND** it MUST NOT infer the layout only from payload byte length

#### Scenario: Unsupported optimized layout fails clearly
- **WHEN** a binary opens an optimized official layout that it does not support
- **THEN** it MUST fail with a clear unsupported-layout error
- **AND** it MUST NOT silently parse the bytes through the generic v13 decode path

### Requirement: Optimized official 3-bit indexes SHALL be rebuild-required
The optimized official `ex_bits=3` direct-IP layouts SHALL be treated as rebuild-required storage layouts. Existing v13 generic official indexes MAY remain readable as fallback inputs, but they MUST NOT be reinterpreted as optimized direct-IP indexes.

#### Scenario: Existing v13 index remains fallback-only
- **WHEN** an existing generic v13 official `1+3` index is used
- **THEN** the query path MAY use the existing decode-to-scratch fallback
- **AND** benchmark metadata MUST identify the run as generic v13 rather than optimized direct compact IP

#### Scenario: Optimized layout build records source and layout
- **WHEN** a new optimized official `1+3` index build completes
- **THEN** index metadata MUST record `rabitq_estimator_mode`, `rabitq_total_bits`, `rabitq_ex_bits`, and the optimized layout key
- **AND** the metadata MUST be sufficient to reproduce whether the index used `2-bit + 1-bit` or `1-bit + 1-bit + 1-bit`
