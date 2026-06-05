## MODIFIED Requirements

### Requirement: Full `.clu` preload SHALL be supported as a query-time cluster-data mode
The system SHALL use full `.clu` preload as the required cluster-side query-data mode for formal query execution. The system SHALL read the complete `.clu` file before measured queries or otherwise ensure resident cluster views are available before probing begins.

#### Scenario: Preload succeeds before query execution
- **WHEN** a benchmark or online serving run starts a measured query batch
- **THEN** the system SHALL ensure the complete `.clu` file has been preloaded before executing the measured queries
- **AND** the resulting resident state SHALL remain available for subsequent queries in that run

#### Scenario: Query rejects missing resident cluster state clearly
- **WHEN** a query is invoked without resident cluster views being available
- **THEN** the system SHALL either perform an explicit lazy preload before probing or fail with a clear error
- **AND** it SHALL NOT silently fall back to window cluster block reads

### Requirement: Resident cluster views SHALL be materialized during preload
The preload path SHALL materialize per-cluster resident views that are sufficient for probe-time access to quantized codes and address-related metadata without issuing query-time `.clu` reads.

#### Scenario: Resident cluster view is available for a probed cluster
- **WHEN** a query probes a cluster after full `.clu` preload has completed
- **THEN** the probe path SHALL obtain that cluster's codes and address-related metadata from resident memory
- **AND** it SHALL NOT require a fresh `.clu` block read for that cluster

#### Scenario: Preload state is reused across queries
- **WHEN** multiple queries run against the same opened index in one benchmark process
- **THEN** resident cluster views SHALL be reused across queries
- **AND** query execution SHALL NOT rebuild query-independent resident cluster metadata per query

### Requirement: Full preload SHALL preserve search semantics
Using full preload as the only cluster-side query mode SHALL NOT change the search semantics of the same operating point.

#### Scenario: Same search parameters preserve result semantics
- **WHEN** the benchmark is run before and after window-read removal with the same dataset, index, query set, `nprobe`, `topk`, and SafeIn/SafeOut parameters
- **THEN** both runs SHALL use the same recall and top-k result definitions
- **AND** any observed performance difference SHALL come from the cluster loading path cleanup rather than a changed search configuration

### Requirement: Payload I/O SHALL remain on the normal query path
The full `.clu` preload mode SHALL preload only cluster-side quantized-vector and address-related state, while leaving payload or raw-vector body reads on the existing query pipeline.

#### Scenario: Payload bytes are not preloaded by cluster preload
- **WHEN** full `.clu` preload is used
- **THEN** the system SHALL continue to fetch payload or raw-vector bodies through the normal query-time path
- **AND** `.clu` preload SHALL NOT be treated as a full payload cache
