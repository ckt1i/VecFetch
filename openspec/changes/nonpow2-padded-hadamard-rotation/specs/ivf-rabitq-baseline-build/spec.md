## MODIFIED Requirements

### Requirement: The build path SHALL encode cluster records with the official RaBitQ library
The baseline build path SHALL rely on the official `RaBitQ-Library` for compressed code generation rather than reusing BoundFetch-specific query logic. When padded-Hadamard mode is enabled for a non-power-of-two `logical_dim`, the build path MUST encode records in `effective_dim = next_power_of_two(logical_dim)` space after zero-padding the tail dimensions, and MUST keep the original `logical_dim` visible in metadata.

#### Scenario: Cluster-local RaBitQ codes are produced during build
- **WHEN** vectors are assigned to their coarse clusters
- **THEN** the build pipeline SHALL encode the assigned records into RaBitQ-compatible compressed representations
- **AND** the resulting codes SHALL be persisted into `clusters.clu`

#### Scenario: Padded-Hadamard build encodes with effective dimension
- **WHEN** padded-Hadamard mode is enabled for a non-power-of-two dataset
- **THEN** the build path MUST zero-pad every encoded vector from `logical_dim` to `effective_dim`
- **AND** the persisted code layout MUST correspond to `effective_dim`
- **AND** metadata MUST continue to record the original `logical_dim`

### Requirement: The build parameters remain visible in metadata
When a baseline index is written, `meta.json` SHALL record the effective RaBitQ-related build parameters and SHALL be sufficient to identify how the codes were produced. For padded-Hadamard builds, metadata MUST additionally record `logical_dim`, `effective_dim`, `padding_mode`, and `rotation_mode`.

#### Scenario: Build parameters remain visible in metadata
- **WHEN** an experiment script inspects a built output directory
- **THEN** it SHALL be able to recover at least the dataset-independent build controls such as `nlist`, metric, dimensionality, record count, and output version

#### Scenario: Padded-Hadamard metadata is self-describing
- **WHEN** a padded-Hadamard build output is inspected
- **THEN** the metadata MUST expose both `logical_dim` and `effective_dim`
- **AND** it MUST expose `padding_mode=zero_pad_to_pow2` or an equivalent explicit label
- **AND** it MUST expose `rotation_mode=hadamard_padded` or an equivalent explicit label
