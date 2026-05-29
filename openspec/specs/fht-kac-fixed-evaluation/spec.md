## ADDED Requirements

### Requirement: The project SHALL define one fixed-parameter MSMARCO comparison for FhtKac rotation
The change SHALL include a single fixed-parameter evaluation contract that compares `fht_kac_rotator` against `hadamard_padded` and `blocked_hadamard_permuted` on MSMARCO.

#### Scenario: Fixed comparison matrix is explicit
- **WHEN** the evaluation contract is executed
- **THEN** it SHALL use the MSMARCO embeddings rooted at `/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings`
- **AND** it SHALL keep `nlist = 16384`, `nprobe = 256`, `bits = 4`, `topk = 10`, `queries = 1000`

### Requirement: The fixed-parameter comparison SHALL keep query-mode controls aligned across modes
The fixed-parameter experiment SHALL keep the query-mode controls aligned across all compared rotation modes so that rotation quality is the primary experimental variable.

#### Scenario: Resident full-preload query settings are shared
- **WHEN** the fixed-parameter comparison is run
- **THEN** each compared mode SHALL use `clu-read-mode = full_preload`
- **AND** each compared mode SHALL use `use-resident-clusters = 1`
- **AND** each compared mode SHALL use `early_stop = 0`

### Requirement: The fixed-parameter comparison SHALL report latency, recall, and storage
The experiment output SHALL report the end-to-end latency, recall, and index-size fields needed to compare the three rotation modes at the fixed operating point.

#### Scenario: Output includes serving metrics
- **WHEN** the fixed-parameter comparison completes
- **THEN** the reported metrics SHALL include `avg_query_time_ms` and `recall_at_10`

#### Scenario: Output includes storage metrics
- **WHEN** the fixed-parameter comparison completes
- **THEN** the reported metrics SHALL include `index_cluster_clu_bytes`
- **AND** it SHALL include `index_rotated_centroids_bytes`
- **AND** it SHALL include `index_total_bytes`
