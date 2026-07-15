## ADDED Requirements

### Requirement: Payload pipeline supports sidecar cold payload slices
The payload pipeline SHALL keep supporting Phase 1 sidecar payload bytes stored
in a cold payload plane while preserving existing inline combined-store
behavior.

#### Scenario: SafeIn reads sidecar cold payload prefix
- **WHEN** a SafeIn candidate uses a Phase 1 sidecar hot/cold record store
- **THEN** the scheduler MUST read the raw vector from the hot vector plane
- **AND** it MUST read at most the configured SafeIn payload prefix from the
  cold payload plane
- **AND** it MUST record prefix/full/suffix counters using the same semantics as
  existing prefix-threshold runs

#### Scenario: Final payload fetch completes sidecar cached prefix
- **WHEN** a final top-k result already has a cached sidecar cold payload prefix
- **THEN** final materialization MUST read only the missing suffix bytes from
  the cold payload plane
- **AND** the assembled payload bytes MUST match the original combined record
  payload

### Requirement: Payload pipeline supports inline hot-record descriptors
The payload pipeline SHALL use inline hot-record descriptors to decide whether
payload bytes are available in the hot record or must be read from
`payload.cold.dat`.

#### Scenario: Inline payload is assembled from the hot record
- **WHEN** a final top-k result has descriptor type `inline_payload`
- **THEN** final materialization MUST read payload bytes from the hot record
- **AND** it MUST NOT issue a cold payload read for that result

#### Scenario: Cold payload is deferred during SafeIn
- **WHEN** a SafeIn candidate has descriptor type `cold_pointer`
- **THEN** the scheduler MUST read raw vector and descriptor bytes from the hot
  record
- **AND** it MUST NOT read payload bytes from `payload.cold.dat` during SafeIn
- **AND** it MUST cache the descriptor so final materialization can read the
  payload only if the candidate survives into the final top-k

#### Scenario: Cold payload is read after final top-k
- **WHEN** a final top-k result has descriptor type `cold_pointer`
- **THEN** final materialization MUST read the payload from `payload.cold.dat`
  using the descriptor offset and length
- **AND** the assembled payload MUST match the original payload bytes

#### Scenario: Unknown descriptor type fails fast
- **WHEN** payload materialization sees an unknown payload descriptor type
- **THEN** it MUST fail the query with a clear layout error
- **AND** it MUST NOT return partial or incorrect payload bytes

### Requirement: Existing inline payload behavior remains unchanged
Existing combined-store records SHALL continue to read payload bytes from
`data.dat` with no hot/cold dependency.

#### Scenario: Combined store runs without hot/cold files
- **WHEN** query is run without a sidecar hot/cold or inline hot-record argument
- **THEN** the scheduler MUST use the existing `data.dat` inline record path
- **AND** benchmark JSON MUST continue to report `record_layout=combined`
