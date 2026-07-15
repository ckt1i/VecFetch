## ADDED Requirements

### Requirement: Sidecar hot/cold record store materialization
The system SHALL keep the Phase 1 sidecar hot/cold store as a benchmarkable
layout for comparison against combined, separate-store, and inline hot-record
layouts.

#### Scenario: Sidecar materializer emits required files
- **WHEN** the sidecar materializer is run with `--index-dir` and `--output`
- **THEN** it MUST create `hotvec.dat`, `payload.cold.dat`, and `hotcold_map.bin`
- **AND** `hotvec.dat` MUST contain one fixed-width raw vector for each unique
  combined record offset
- **AND** `payload.cold.dat` MUST contain the corresponding payload bytes
- **AND** `hotcold_map.bin` MUST map each combined record offset to a hot vector
  row and a cold payload slice

#### Scenario: Sidecar materialization preserves record cardinality
- **WHEN** duplicate cluster references point to the same combined record offset
- **THEN** the materializer MUST write that record exactly once to the sidecar
  hot/cold store
- **AND** the map MUST contain one entry for the unique combined record offset

### Requirement: Inline hot-record store materialization
The system SHALL materialize an inline hot-record store whose cluster record
addresses directly point to hot records and do not require `hotcold_map.bin` at
query time.

#### Scenario: Inline materializer emits required files
- **WHEN** the inline hot-record materializer is run with an existing RecordGate
  index and an output directory
- **THEN** it MUST create a derived index directory containing rewritten record
  addresses
- **AND** it MUST create a hot record file containing raw vectors followed by a
  payload descriptor and optional inline payload bytes
- **AND** it MUST create `payload.cold.dat` when at least one payload is stored
  cold
- **AND** it MUST NOT require `hotcold_map.bin` for query execution

#### Scenario: Inline materializer preserves quantized search artifacts
- **WHEN** the inline materializer creates a derived index
- **THEN** it MUST preserve IVF/RaBitQ quantized cluster data and search
  parameters
- **AND** it MUST only rewrite record-address metadata and record storage bytes
  needed for direct hot-record access

#### Scenario: Inline materializer records layout metadata
- **WHEN** inline materialization completes
- **THEN** the manifest MUST include `layout=inline_hot_record_store`
- **AND** it MUST include descriptor size, inline payload threshold, record
  count, inline record count, cold record count, hot record bytes, cold payload
  bytes, `effective_safein_inline_threshold`, and `address_map_bytes=0`

### Requirement: Hot payload descriptor
The inline hot-record format SHALL encode payload placement in a descriptor that
is stored immediately after the raw vector bytes.

#### Scenario: Descriptor identifies inline payload
- **WHEN** a record's payload is no larger than the inline threshold
- **THEN** its descriptor MUST identify the payload as inline
- **AND** the payload bytes MUST be readable from the hot record without opening
  or reading `payload.cold.dat`

#### Scenario: Descriptor identifies cold payload
- **WHEN** a record's payload is larger than the inline threshold
- **THEN** its descriptor MUST identify the payload as cold
- **AND** it MUST contain the cold payload offset and full payload byte length
- **AND** the hot record MUST NOT duplicate the full cold payload body
- **AND** SafeIn MUST NOT prefetch cold payload bytes for that record before
  final top-k materialization

#### Scenario: Descriptor remains extensible
- **WHEN** the descriptor is serialized
- **THEN** the payload placement field MUST be at least one byte
- **AND** values for inline and cold pointer payloads MUST be explicitly
  versioned or validated
- **AND** unknown placement values MUST be rejected before query reads payload
  bytes

### Requirement: Inline store metadata validation
The inline hot-record store SHALL be self-describing enough for query
benchmarks to reject incompatible stores before issuing reads.

#### Scenario: Query rejects incompatible vector width
- **WHEN** query opens an inline hot-record store whose manifest or descriptor
  vector byte width does not match the index logical vector width
- **THEN** query initialization MUST fail with a clear error

#### Scenario: Query identifies inline hot-record layout
- **WHEN** a query benchmark runs with an inline hot-record store
- **THEN** its JSON metrics MUST report `record_layout=inline_hot_record_store`
- **AND** the output MUST include the inline hot-record store directory used
  for the run
- **AND** diagnostics MUST report that no sidecar address map was loaded

### Requirement: Direct cluster-address access without sidecar map
The inline hot-record store SHALL preserve RecordGate's cluster-addressed access
model without a global combined-offset map.

#### Scenario: Query reads hot record directly from cluster address
- **WHEN** `OverlapScheduler` receives an `AddressEntry` from the derived
  inline hot-record index
- **THEN** `AddressEntry.offset` MUST be the hot record offset
- **AND** the scheduler MUST read raw vector and descriptor bytes directly from
  that offset
- **AND** it MUST NOT perform a `combined_offset -> row_id` lookup

#### Scenario: Missing or stale sidecar map cannot affect inline query
- **WHEN** query is run in inline hot-record mode
- **THEN** query correctness MUST NOT depend on `hotcold_map.bin`
- **AND** deleting any Phase 1 sidecar map file MUST NOT change inline query
  results

### Requirement: Large-payload experiment artifacts
The change SHALL produce reproducible VoxCeleb2 and MSMARCO experiment artifacts
under `/home/zcq/VDB/test`.

#### Scenario: Stores are placed under dataset test data directories
- **WHEN** sidecar or inline hot/cold stores are materialized for VoxCeleb2 and
  MSMARCO
- **THEN** they MUST be placed under
  `/home/zcq/VDB/test/data/<dataset>/indexes/recordgate/`
- **AND** the output directory names MUST identify the hot/cold format variant

#### Scenario: Results are placed under a dedicated test directory
- **WHEN** inline hot-record query experiments complete
- **THEN** raw JSON, CSV summaries, logs, and a Chinese result summary MUST be
  stored under one dedicated
  `/home/zcq/VDB/test/recordgate_inline_hot_record_store_*` directory
