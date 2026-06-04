## MODIFIED Requirements

### Requirement: The project SHALL define one fixed-parameter MSMARCO comparison for FhtKac rotation
The project SHALL define a fixed-parameter MSMARCO validation contract for the FHT-Kac mainline rotation path. The contract SHALL no longer require comparison against `hadamard_padded` or `blocked_hadamard_permuted`.

#### Scenario: Fixed FHT-Kac validation is explicit
- **WHEN** the FHT-Kac validation contract is executed
- **THEN** it SHALL use the MSMARCO embeddings or adapter configured by the current test_config
- **AND** it SHALL keep the documented `nlist`, `nprobe`, `bits`, `topk`, and query count for the mainline operating point

#### Scenario: Removed rotation modes are not part of the fixed matrix
- **WHEN** the FHT-Kac validation report is generated
- **THEN** it SHALL NOT require `hadamard_padded` or `blocked_hadamard_permuted` rows
- **AND** conclusions SHALL be based on the FHT-Kac mainline result

### Requirement: The fixed-parameter comparison SHALL report latency, recall, and storage
The FHT-Kac validation output SHALL report the end-to-end latency, recall, and index-size fields needed to validate the mainline non-power-of-two rotation path.

#### Scenario: Output includes serving metrics
- **WHEN** the fixed-parameter FHT-Kac validation completes
- **THEN** the reported metrics SHALL include `avg_query_time_ms` and `recall_at_10`

#### Scenario: Output includes storage metrics
- **WHEN** the fixed-parameter FHT-Kac validation completes
- **THEN** the reported metrics SHALL include `index_cluster_clu_bytes`
- **AND** it SHALL include `index_rotated_centroids_bytes`
- **AND** it SHALL include `index_total_bytes`
