## ADDED Requirements

### Requirement: ExRaBitQ cleanup SHALL preserve frozen and baseline layouts
The cleanup SHALL preserve support for the frozen method layout and the official-like baseline or compatibility layout. In particular, `tile_lane_bitmajor` and `vector_bitplanes` SHALL remain usable for reading, benchmarking, and validation unless a later migration change explicitly removes compatibility.

#### Scenario: Frozen method layout remains readable
- **WHEN** a query loads an existing index written with the frozen method layout
- **THEN** the reader MUST parse the layout successfully
- **AND** the query path MUST execute with the same fixed active ex-bits semantics as before cleanup

#### Scenario: Official-like baseline layout remains available
- **WHEN** a benchmark compares against the official-like RabitQ baseline layout
- **THEN** the corresponding layout MUST remain available
- **AND** cleanup MUST NOT silently remap it to the method layout

### Requirement: ExRaBitQ cleanup SHALL use conservative SIMD removal
This cleanup SHALL NOT delete low-level SIMD helper or kernel implementations solely because abandoned upper-level features no longer call them. SIMD deletion MAY occur only in a later change after proving that the helper is unused by frozen, baseline, compatibility, and test paths.

#### Scenario: Upper-level progressive calls are removed but SIMD helpers remain
- **WHEN** progressive pruning is removed from the query pipeline
- **THEN** upper-level calls from serving and benchmark paths MUST be removed
- **AND** shared SIMD helpers MAY remain compiled and tested

### Requirement: ExRaBitQ cleanup SHALL not require index rebuild
The cleanup SHALL be an implementation and API-surface cleanup, not a storage migration. Existing indexes using the frozen or baseline layouts MUST NOT require rebuild only because abandoned query features were removed.

#### Scenario: Existing frozen-format index is reused after cleanup
- **WHEN** validation reruns on a previously built frozen-format index
- **THEN** the query must load the index without rebuild
- **AND** recall and speed must be compared under the same physical index files
