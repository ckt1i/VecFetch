## ADDED Requirements

### Requirement: Non-power-of-two dimensions SHALL support padded Hadamard rotation as an explicit experimental mode
The system SHALL support an experimental rotation mode that zero-pads a vector with `logical_dim` to `effective_dim = next_power_of_two(logical_dim)` and then applies the existing Hadamard rotation path on the padded vector. The original `logical_dim` MUST remain visible in metadata and benchmark outputs.

#### Scenario: A 768-dim query is padded to 1024 before Hadamard rotation
- **WHEN** an index is built or queried with padded-Hadamard mode enabled for `logical_dim=768`
- **THEN** the system MUST set `effective_dim=1024`
- **AND** the tail dimensions `768..1023` MUST be zero-filled before rotation
- **AND** the index metadata MUST still record `logical_dim=768`

### Requirement: Padded Hadamard mode SHALL preserve build/query symmetry
When padded-Hadamard mode is enabled, the same `logical_dim`, `effective_dim`, padding rule, and rotation mode MUST be used consistently for base vectors, centroids, and query vectors. The build path MUST produce artifacts compatible with the query path without falling back to the random-rotation interpretation.

#### Scenario: Build and query reopen the same padded-Hadamard index
- **WHEN** a padded-Hadamard index is built and later reopened for serving
- **THEN** the query path MUST load the same `logical_dim`, `effective_dim`, and rotation mode from metadata
- **AND** the query path MUST use the pre-rotated query preparation path rather than the random-rotation matrix path

### Requirement: Padded Hadamard mode SHALL persist rotated-centroid artifacts for the padded dimension
The padded-Hadamard build path MUST persist rotated centroids computed in `effective_dim` space so that query serving can reuse the pre-rotated residual preparation path.

#### Scenario: Rotated centroids are present for padded-Hadamard reopen
- **WHEN** a padded-Hadamard index build completes successfully
- **THEN** the output directory MUST include the rotated-centroid artifact for `effective_dim`
- **AND** reopening the index MUST make that artifact available to the serving path
