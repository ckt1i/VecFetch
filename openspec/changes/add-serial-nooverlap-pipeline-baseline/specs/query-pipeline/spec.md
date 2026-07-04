## ADDED Requirements

### Requirement: Query pipeline SHALL support a serial no-overlap execution mode
The query pipeline SHALL provide a `serial_no_overlap` execution mode for resident RecordGate queries. This mode MUST reuse the same index, coarse routing, RaBitQ Stage1/Stage2 probing, SafeOut/SafeIn classification, candidate budget, deduplication and final top-k semantics as the default overlap pipeline. It MUST differ only in execution scheduling: probe MUST finish candidate collection before any raw-vector or payload data read is issued.

#### Scenario: Serial mode collects candidates before data I/O
- **WHEN** a resident query runs with `execution_mode=serial_no_overlap`
- **THEN** the pipeline MUST complete coarse routing and probing for all selected clusters before issuing raw-vector, full-record or payload reads
- **AND** the probe phase MUST NOT call async data `PrepRead`, `Submit`, `Poll` or `WaitAndPoll` for candidate data

#### Scenario: Serial mode preserves candidate semantics
- **WHEN** the same query, index and search parameters are run under default overlap mode and serial no-overlap mode
- **THEN** both modes MUST use the same SafeOut/SafeIn frontier logic and candidate budget policy
- **AND** both modes MUST produce matching probed counts, SafeOut/SafeIn/Uncertain counts and rerank candidate counts within benchmark tolerance

### Requirement: Serial no-overlap mode SHALL execute read plans synchronously after probe
After probe finishes, `serial_no_overlap` mode SHALL materialize the same candidate read plans that the overlap path would execute and SHALL read them synchronously. `VEC_ONLY` plans MUST read only raw-vector bytes, `VEC_ALL` plans MUST read the full record and preserve SafeIn payload-cache semantics, and final payload reads MUST happen only after exact rerank finalizes top-k.

#### Scenario: Vector-only plans are read synchronously
- **WHEN** a collected candidate materializes as `VEC_ONLY`
- **THEN** serial mode MUST synchronously read exactly the raw-vector byte range from `data.dat` or the configured separate vector store
- **AND** the read vector MUST be passed to the same exact rerank consumer semantics as the overlap path

#### Scenario: SafeIn full-record plans are preserved
- **WHEN** a collected candidate materializes as `VEC_ALL` under the same access policy used by Full Pipeline
- **THEN** serial mode MUST synchronously read the full record byte range
- **AND** the raw vector MUST be exact-reranked
- **AND** the payload portion MUST be cached for final result assembly using semantics equivalent to the overlap path

#### Scenario: Final payload materialization remains after exact rerank
- **WHEN** exact rerank finalizes the query top-k
- **THEN** serial mode MUST synchronously read only payloads missing from the payload cache
- **AND** result assembly MUST produce the same payload schema and ordering semantics as the overlap path

### Requirement: Serial no-overlap mode SHALL not use speculative prefetch
Serial no-overlap mode MUST disable speculative or budgeted prefetch mechanisms that issue candidate data reads before final read-plan materialization. If a benchmark command supplies a speculative prefetch limit, the effective value in serial mode MUST be zero or the command MUST fail with a clear error.

#### Scenario: Budgeted prefetch is disabled in serial mode
- **WHEN** a query runs with `execution_mode=serial_no_overlap`
- **THEN** the effective `budgeted_prefetch_limit` MUST be zero
- **AND** no `SPEC_VEC_ONLY` request MUST be emitted

### Requirement: Serial no-overlap mode SHALL preserve batch exact-rerank semantics
Serial no-overlap mode SHALL preserve the existing exact-rerank computation semantics, including batch SIMD rerank when enabled. This mode is a scheduling ablation and MUST NOT implicitly downgrade exact rerank to scalar per-vector computation.

#### Scenario: Batch rerank remains available
- **WHEN** serial mode has synchronously read all selected raw vectors
- **THEN** it MAY pass those vectors to the existing batched rerank consumer
- **AND** it MUST report exact rerank counts and distances under the same semantics as overlap mode
