## ADDED Requirements

### Requirement: Compact layout SHALL support official 3-bit `2-bit + 1-bit` ExData
The compact Stage2 layout SHALL support an official `ex_bits=3` ExData representation that splits each 64-dimensional chunk into a 16-byte low-2-bit payload and an 8-byte high-1-bit payload.

#### Scenario: Builder writes 2-plus-1 chunks
- **WHEN** the builder serializes an official `total_bits=4, ex_bits=3` index with the `2-bit + 1-bit` layout
- **THEN** each 64-dimensional ExData chunk for one lane MUST contain 16 bytes of low-2-bit compact code
- **AND** it MUST contain 8 bytes of high-1-bit compact code
- **AND** the combined decoded code for each dimension MUST equal the scalar sign-folded ExData code

#### Scenario: Resident view exposes 2-plus-1 chunks without full decode
- **WHEN** resident preload exposes a `2-bit + 1-bit` Stage2 batch block
- **THEN** the parsed view MUST provide direct compact pointers and stride metadata for the low-2-bit and high-1-bit payloads
- **AND** it MUST NOT require a resident `uint8_t[dim]` decoded ExData buffer per lane

### Requirement: Compact layout SHALL support official 3-bit three-bitplane ExData
The compact Stage2 layout SHALL support an official `ex_bits=3` ExData representation that stores three independent 1-bit planes for each 64-dimensional chunk.

#### Scenario: Builder writes three bitplanes
- **WHEN** the builder serializes an official `total_bits=4, ex_bits=3` index with the `1-bit + 1-bit + 1-bit` layout
- **THEN** each 64-dimensional ExData chunk for one lane MUST contain three 8-byte bitplanes
- **AND** the combined code `b0 + 2*b1 + 4*b2` MUST equal the scalar sign-folded ExData code for every dimension

#### Scenario: Resident view exposes bitplanes without full decode
- **WHEN** resident preload exposes a three-bitplane Stage2 batch block
- **THEN** the parsed view MUST provide direct compact pointers and stride metadata for all three planes
- **AND** it MUST NOT require a resident `uint8_t[dim]` decoded ExData buffer per lane

### Requirement: Non-selected compact layout SHALL not be the default serving layout
After both optimized official 3-bit layouts have been benchmarked, the compact layout selected by the acceptance comparison SHALL be the default optimized serving layout. The non-selected layout SHALL either be removed from the serving fast path or marked as experimental-only.

#### Scenario: Selected layout becomes default
- **WHEN** both optimized official 3-bit layout benchmark results are available and one layout wins under the selection rule
- **THEN** default optimized official `ex_bits=3` index builds MUST use the winning layout
- **AND** benchmark metadata MUST record that the layout is selected

#### Scenario: Non-selected layout is not used accidentally
- **WHEN** the non-selected layout remains in the codebase for tests or diagnostics
- **THEN** it MUST require an explicit experimental layout selection
- **AND** it MUST NOT be used by default official `1+3` builds or main-result runs
