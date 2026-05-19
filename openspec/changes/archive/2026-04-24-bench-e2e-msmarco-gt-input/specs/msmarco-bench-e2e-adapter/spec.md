## ADDED Requirements

### Requirement: Temporary MSMARCO adapter SHALL emit a COCO-style benchmark directory
The adapter SHALL convert MSMARCO embeddings and raw text assets into the directory structure expected by `bench_e2e`.

#### Scenario: Adapter is generated
- **WHEN** the adapter script is run for MSMARCO
- **THEN** it SHALL produce a temporary directory containing benchmark-compatible embedding, id, and metadata files

#### Scenario: Adapter preserves source assets
- **WHEN** the adapter is generated
- **THEN** the original MSMARCO formal-baseline assets SHALL remain unchanged

### Requirement: Adapter SHALL map MSMARCO passages and queries to benchmark files
The adapter SHALL create benchmark inputs that correspond to passage embeddings, query embeddings, and identifier arrays.

#### Scenario: Passage embeddings are adapted
- **WHEN** MSMARCO base embeddings are ingested
- **THEN** the adapter SHALL expose them under the benchmark's base-embedding file name

#### Scenario: Query embeddings are adapted
- **WHEN** MSMARCO query embeddings are ingested
- **THEN** the adapter SHALL expose them under the benchmark's query-embedding file name

#### Scenario: Identifier arrays are materialized
- **WHEN** the adapter is generated
- **THEN** it SHALL write stable passage and query identifier arrays aligned with the embeddings

### Requirement: Adapter SHALL provide metadata for benchmark compatibility
The adapter SHALL emit a metadata file that satisfies the benchmark's current payload lookup path.

#### Scenario: Passage text is available
- **WHEN** MSMARCO collection text is present
- **THEN** the adapter SHALL emit per-passage metadata records compatible with the benchmark's expected metadata format

### Requirement: Adapter SHALL support a precomputed ground-truth artifact
The adapter SHOULD place a precomputed ground-truth artifact alongside the benchmark inputs so the benchmark can reuse it without brute-force generation.

#### Scenario: GT artifact is present
- **WHEN** the adapter is generated with a precomputed ground-truth file
- **THEN** the ground-truth artifact SHALL be copied or linked into the adapter directory
- **AND** its location SHALL be recorded in the adapter manifest

