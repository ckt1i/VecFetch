## MODIFIED Requirements

### Requirement: Cluster-side query data SHALL support selectable loading modes
The cluster-side query path SHALL no longer support selectable cluster loading modes during formal query execution. The only supported cluster-side query data mode SHALL be full `.clu` preload with resident parsed cluster views; window cluster reads SHALL be removed from the online query path.

#### Scenario: Full preload mode is the only formal query mode
- **WHEN** a query benchmark or online search is executed
- **THEN** the query pipeline SHALL obtain cluster-side probe data from resident memory
- **AND** it SHALL NOT submit per-cluster `CLUSTER_BLOCK` I/O requests during the measured query phase

#### Scenario: Window mode is not a selectable benchmark mode
- **WHEN** a benchmark or script configures cluster loading
- **THEN** it SHALL NOT expose `window` as a valid formal serving mode
- **AND** it SHALL NOT require `prefetch_depth`, `refill_threshold`, or `refill_count` to reproduce the official query path

#### Scenario: Loading-mode comparison is no longer required
- **WHEN** the benchmark records query configuration metadata
- **THEN** the output SHALL make the resident full-preload assumption clear
- **AND** it SHALL NOT report a window/full-preload comparison as the objective of this path

### Requirement: Query-time cluster probe SHALL remain compatible with the parsed-cluster access pattern
The query-time cluster probe path SHALL consume resident cluster representations that expose the probe-relevant fields required by the parsed-cluster-based logic. Removing window reads SHALL NOT require a different search algorithm or a different top-k interpretation.

#### Scenario: Probe path uses resident parsed cluster representation
- **WHEN** a full-preload resident query probes a cluster
- **THEN** the cluster probe implementation SHALL receive a resident cluster representation compatible with the existing quantized-code and decoded-address access pattern
- **AND** the change SHALL NOT require a different SafeIn/SafeOut/Uncertain interpretation

#### Scenario: Top-k semantics remain unchanged
- **WHEN** the same index, query set, `nprobe`, and `topk` are evaluated before and after window-read removal
- **THEN** final results SHALL keep the same top-k ordering semantics
- **AND** any performance difference SHALL come from removing cluster-side window I/O and its control flow, not from a changed search definition

## REMOVED Requirements

### Requirement: Sliding-window cluster block prefetch and refill
**Reason**: The project no longer treats query-time window cluster reads as a formal serving path. Keeping `prefetch_depth`, `refill_threshold`, and `refill_count` would preserve an unused branch and misleading tuning knobs.

**Migration**: Use full `.clu` preload and resident parsed cluster views before running queries. Existing indexes remain reusable because the `.clu` file format is unchanged.

### Requirement: Query-time `CLUSTER_BLOCK` completion handling
**Reason**: Resident full preload eliminates per-cluster block submissions in the measured query phase, so `CLUSTER_BLOCK` completions are no longer part of the query event loop.

**Migration**: Completion dispatch SHALL retain vector and payload request types only; cluster parsing happens during preload or resident view construction rather than per query.
