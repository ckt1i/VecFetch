## ADDED Requirements

### Requirement: ImageNet and VoxCeleb2 SHALL be prepared as formal baseline datasets without Parquet backend readiness
The formal baseline data-preparation pipeline SHALL make `imagenet1k` and `voxceleb2_ecapa_150k` runnable using the existing raw and formatted dataset planes, while requiring only FlatStor and Lance payload backends for this change.

#### Scenario: ImageNet formal assets are materialized
- **WHEN** `imagenet1k` is prepared for ICDE baseline execution
- **THEN** the raw embeddings SHALL be read from `/home/zcq/VDB/data/formal_baselines/imagenet1k/embeddings/`
- **AND** formatted assets SHALL be written under `/home/zcq/VDB/baselines/data/formal_baselines/imagenet1k/`
- **AND** the formatted assets SHALL include cleaned payload metadata, a split manifest, exact ground truth, `payload_flatstor/default`, and `payload_lance/default`
- **AND** the payload records SHALL preserve JPEG bytes or an equivalent raw-record byte payload plus filename, split, label or synset metadata, and row id

#### Scenario: VoxCeleb2 formal assets are materialized
- **WHEN** `voxceleb2_ecapa_150k` is prepared for ICDE baseline execution
- **THEN** the raw embeddings SHALL be read from `/home/zcq/VDB/data/formal_baselines/voxceleb2_ecapa_150k/embeddings/`
- **AND** formatted assets SHALL be written under `/home/zcq/VDB/baselines/data/formal_baselines/voxceleb2_ecapa_150k/`
- **AND** the formatted assets SHALL include cleaned payload metadata, a split manifest, exact ground truth, `payload_flatstor/default`, and `payload_lance/default`
- **AND** the payload records SHALL preserve audio bytes or an equivalent raw-record byte payload plus speaker id, tar path, member name, and row id

#### Scenario: Parquet backend is not required for expansion datasets
- **WHEN** readiness is evaluated for `imagenet1k` or `voxceleb2_ecapa_150k`
- **THEN** the dataset SHALL NOT require `payload_parquet/default` to be considered runnable for this change
- **AND** the readiness report SHALL record the required backend set as `flatstor,lance`
- **AND** a missing Parquet payload backend SHALL NOT block IVF, DiskANN, or aggregation for these datasets

### Requirement: Expansion dataset payload exports SHALL remain row-aligned across FlatStor and Lance
The payload exports for `imagenet1k` and `voxceleb2_ecapa_150k` SHALL preserve a stable mapping between vector row ids and payload positions across FlatStor and Lance.

#### Scenario: Payload row ids match base embeddings
- **WHEN** payload exports are generated for an expansion dataset
- **THEN** every exported payload row SHALL have a `row_id` in the range `[0, base_row_count)`
- **AND** the number of exported payload rows SHALL equal the number of base embeddings
- **AND** FlatStor and Lance SHALL expose the same logical payload for the same `row_id`

#### Scenario: Raw archives are streamed without duplicate extraction
- **WHEN** ImageNet or VoxCeleb2 raw payload bytes are materialized
- **THEN** the implementation SHALL avoid extracting a duplicate full raw file tree
- **AND** it SHALL stream from the existing official tar or WDS archive files when building FlatStor and Lance outputs
- **AND** it SHALL preserve the user-provided raw downloads untouched

### Requirement: Expansion dataset ground truth SHALL be exact top-100 for a fixed query split
The data-preparation pipeline SHALL generate exact nearest-neighbor ground truth for the fixed query split of each expansion dataset.

#### Scenario: Exact ground truth is generated
- **WHEN** `imagenet1k` or `voxceleb2_ecapa_150k` becomes runnable
- **THEN** the pipeline SHALL generate `gt_top10.npy`, `gt_top20.npy`, and `gt_top100.npy` under the dataset's formatted `gt/` directory
- **AND** `gt_top100.npy` SHALL contain exact top-100 neighbors computed with the same normalized cosine or inner-product metric used by the baseline runners

#### Scenario: Query split is reproducible
- **WHEN** exact ground truth is generated for an expansion dataset
- **THEN** the chosen query ids SHALL be recorded in `splits/split_v1.json`
- **AND** subsequent baseline runs SHALL sample queries from that fixed split rather than reselecting ad hoc query ids

