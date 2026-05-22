## ADDED Requirements

### Requirement: The IVF-RaBitQ baseline SHALL build its coarse partitioning with Faiss-compatible semantics
The baseline build path SHALL use Faiss or Faiss-equivalent clustering logic for `nlist` training and vector-to-cluster assignment.

#### Scenario: Build CLI accepts Faiss-style coarse parameters
- **WHEN** a user launches the baseline build command
- **THEN** the command SHALL accept at least `--nlist`, `--metric`, `--train-size`, and `--output`
- **AND** those parameters SHALL determine the coarse quantizer training and output layout

#### Scenario: Coarse centroids are persisted for later probing
- **WHEN** coarse training completes
- **THEN** the resulting coarse centroids SHALL be written to disk in the compressed index plane
- **AND** a later search run SHALL be able to reuse them without retraining

### Requirement: The build path SHALL encode cluster records with the official RaBitQ library
The baseline build path SHALL rely on the official `RaBitQ-Library` for compressed code generation rather than reusing BoundFetch-specific query logic.

#### Scenario: Cluster-local RaBitQ codes are produced during build
- **WHEN** vectors are assigned to their coarse clusters
- **THEN** the build pipeline SHALL encode the assigned records into RaBitQ-compatible compressed representations
- **AND** the resulting codes SHALL be persisted into `clusters.clu`

#### Scenario: Build metadata records the effective encoding configuration
- **WHEN** a baseline index is written
- **THEN** `meta.json` SHALL record the effective RaBitQ-related build parameters
- **AND** those parameters SHALL be sufficient to identify how the codes were produced

### Requirement: The build output SHALL be reproducible and relocatable
The baseline build path SHALL emit a self-describing output directory that can be copied and reopened by the search CLI.

#### Scenario: Output directory contains all required baseline artifacts
- **WHEN** the build command succeeds
- **THEN** the output directory SHALL contain the compressed index plane artifacts and the raw vector plane artifact
- **AND** the output directory SHALL be openable by a later search command without requiring the original input base file

#### Scenario: Build parameters remain visible in metadata
- **WHEN** an experiment script inspects a built output directory
- **THEN** it SHALL be able to recover at least the dataset-independent build controls such as `nlist`, metric, dimensionality, record count, and output version

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

### Requirement: Build parameters remain visible in metadata
When a baseline index is written, `meta.json` SHALL record the effective RaBitQ-related build parameters and SHALL be sufficient to identify how the codes were produced. For padded-Hadamard builds, metadata MUST additionally record `logical_dim`, `effective_dim`, `padding_mode`, and `rotation_mode`.

#### Scenario: Build parameters remain visible in metadata
- **WHEN** an experiment script inspects a built output directory
- **THEN** it SHALL be able to recover at least the dataset-independent build controls such as `nlist`, metric, dimensionality, record count, and output version

#### Scenario: Padded-Hadamard metadata is self-describing
- **WHEN** a padded-Hadamard build output is inspected
- **THEN** the metadata MUST expose both `logical_dim` and `effective_dim`
- **AND** it MUST expose `padding_mode=zero_pad_to_pow2` or an equivalent explicit label
- **AND** it MUST expose `rotation_mode=hadamard_padded` or an equivalent explicit label
