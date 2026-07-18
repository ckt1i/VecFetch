# Spec: Payload Pipeline + Npy Loader
## Requirements

### R1: IvfBuilder accepts per-vector payloads via callback

- `Build()` accepts an optional `PayloadFn` parameter (defaults to `nullptr`).
- When `PayloadFn` is provided, each record in `data.dat` includes payload columns.
- When `PayloadFn` is `nullptr`, behavior is identical to current (empty payload).
- `PayloadFn` signature: `std::function<std::vector<Datum>(uint32_t vec_index)>`
- `config_.payload_schemas` must be set to match the columns returned by `PayloadFn`.

### R2: Payload schemas persisted in segment.meta

- `segment_meta.fbs` includes a `PayloadColumnSchema` table with fields: `id`, `name`, `dtype`, `nullable`.
- `SegmentMeta` includes a `payload_schemas` vector field.
- `IvfBuilder` serializes `config_.payload_schemas` into segment.meta during build.
- Old segment.meta files without `payload_schemas` remain valid (FlatBuffers compatibility).

### R3: IvfIndex::Open() restores payload schemas

- `Open()` reads `payload_schemas` from segment.meta into `payload_schemas_` member.
- `payload_schemas_` is passed to `Segment::Open()` and subsequently to `DataFileReader::Open()`.
- If segment.meta has no `payload_schemas` field, `payload_schemas_` remains empty (backward compatible).

### R4: NpyReader loads float32 and int64 arrays

- `LoadNpyFloat32(path)` returns `StatusOr<NpyArrayFloat>` with row-major data, rows, cols.
- `LoadNpyInt64(path)` returns `StatusOr<NpyArrayInt64>` with data and count.
- Supports npy format version 1.0 and 2.0.
- Returns error for unsupported dtypes, big-endian, or Fortran order.

### R5: JsonlReader iterates lines

- `ReadJsonlLines(path, callback)` reads file line by line, invoking callback with `(line_number, line_content)`.
- Skips empty lines.
- Returns `Status::IOError` if file cannot be opened.

### R6: Backward compatibility

- All existing tests pass without modification (Build() without PayloadFn compiles and works).
- Old segment.meta files (without payload_schemas) load correctly.
- No changes to DataFileWriter, DataFileReader, ClusterStore*, OverlapScheduler, or RerankConsumer.

### R7: Standard two-column payload convention

- All test harnesses use a standard two-column payload layout:
  - Column 0: `{id: 0, name: "id", dtype: INT64}` — record identity for recall verification.
  - Column 1: `{id: 1, name: "data", dtype: BYTES or STRING}` — raw original data.
- Column 1 dtype varies by modality: BYTES for binary data (image/audio/video), STRING for text.
- This convention is not enforced by the engine — payload_schemas is fully flexible.

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

### Requirement: SafeIn optional payload I/O SHALL not bypass mandatory backlog through early-submit
The payload pipeline SHALL schedule optional SafeIn payload I/O through the normal optional-I/O admission, capacity, submit, and drain policy. It MUST NOT allow a first batch of optional requests to bypass a mandatory backlog through `safein_optional_io_early_submit_max_requests` or an equivalent deleted early-submit branch.

#### Scenario: Mandatory backlog blocks optional payload submission
- **WHEN** mandatory vector I/O is above the scheduler's normal optional-I/O admission threshold
- **THEN** newly queued optional SafeIn payload I/O MUST remain pending under the normal policy
- **AND** it MUST NOT be submitted by a bounded early-submit exception

#### Scenario: Optional payload I/O proceeds after normal admission becomes available
- **WHEN** mandatory pressure clears and the ordinary optional-I/O admission conditions are satisfied
- **THEN** eligible SafeIn payload I/O MUST be allowed to submit through the normal optional-I/O path
- **AND** removal of early-submit MUST NOT disable optional payload prefetch as a whole

### Requirement: Removing optional early-submit SHALL preserve final payload completeness
The payload pipeline MUST preserve final top-k payload materialization regardless of whether an optional SafeIn payload was prefetched. Any payload not available from span reuse or normal optional prefetch MUST be fetched by the final materialization path.

#### Scenario: Deferred optional payload is fetched at finalization
- **WHEN** a final top-k record's payload was not submitted through normal optional prefetch
- **THEN** final materialization MUST fetch the missing payload
- **AND** the returned record contents MUST match the reference no-cap execution

## Acceptance Criteria

1. Unit test: Build with PayloadFn (INT64 id + BYTES image) → Open → ReadRecord → payload[0] is correct id, payload[1] is correct image bytes.
2. Unit test: Build without PayloadFn → Open → results have empty payload (backward compat).
3. Unit test: LoadNpyFloat32 reads coco_1k/image_embeddings.npy → shape (1000, 512).
4. Unit test: LoadNpyInt64 reads coco_1k/image_ids.npy → count 1000.
5. Unit test: ReadJsonlLines reads coco_1k/metadata.jsonl → 1000 lines.
6. Integration: Build coco_1k (1000 vectors, dim=512, nlist=32, payload=[id INT64, image BYTES]) → Search → result payload[0] matches ground truth image_id, payload[1] is valid jpg bytes.
