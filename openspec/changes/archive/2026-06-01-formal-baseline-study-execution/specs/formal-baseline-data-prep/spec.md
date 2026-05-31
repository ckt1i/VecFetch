## ADDED Requirements

### Requirement: Formal baseline datasets SHALL follow a fixed storage contract
The formal baseline study SHALL separate raw assets from experiment-formatted assets by root path, and every dataset in the study SHALL use the same directory contract.

#### Scenario: Raw assets are placed under the raw plane
- **WHEN** a dataset is downloaded, linked, or embedded for the formal baseline study
- **THEN** its raw files and raw embeddings SHALL be stored under `/home/zcq/VDB/data/formal_baselines/<dataset>/`
- **AND** that directory SHALL only contain `raw/`, `embeddings/`, and `logs/` style assets

#### Scenario: Derived assets are placed under the formatted plane
- **WHEN** cleaned payload tables, split manifests, ground truth, or backend-formatted payloads are generated
- **THEN** they SHALL be stored under `/home/zcq/VDB/baselines/data/formal_baselines/<dataset>/`
- **AND** that directory SHALL provide `cleaned/`, `splits/`, `gt/`, and `payload_*` outputs as applicable

#### Scenario: Third-party libraries use the dedicated dependency root
- **WHEN** a manual download or source build is required for a baseline dependency
- **THEN** that dependency SHALL be placed under `/home/zcq/VDB/third_party/<library>/`
- **AND** the baseline registry SHALL record the installation path and version or commit

### Requirement: Primary datasets SHALL be materialized with dataset-specific assets
The formal baseline study SHALL prepare the four primary datasets with the assets required by the selected baselines and the coupled E2E protocol.

#### Scenario: COCO 100K is prepared from existing local assets
- **WHEN** `coco_100k` is initialized for the study
- **THEN** the pipeline SHALL accept existing local image and query embeddings
- **AND** it SHALL generate `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/cleaned/payload.parquet` and `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/splits/split_v1.json`

#### Scenario: MS MARCO Passage is prepared from official raw data
- **WHEN** `msmarco_passage` is initialized for the study
- **THEN** the pipeline SHALL download or ingest official passages, queries, and qrels
- **AND** it SHALL generate cleaned passage/query parquet files plus a fixed split manifest under `/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/`

#### Scenario: Deep8M synthetic payload is prepared from existing vector assets
- **WHEN** `deep8m_synth` is initialized for the study
- **THEN** the pipeline SHALL link or copy the existing base/query/groundtruth vector assets
- **AND** it SHALL generate deterministic synthetic payload tables for `256B`, `4KB`, and `64KB`

#### Scenario: Amazon ESCI is prepared from official product-search assets
- **WHEN** `amazon_esci` is initialized for the study
- **THEN** the pipeline SHALL ingest official queries, products, and relevance labels
- **AND** it SHALL generate cleaned `products`, `queries`, and `labels` parquet files plus a fixed split manifest

### Requirement: Shared embeddings and ground truth SHALL be generated with a single study-wide contract
All baselines in the formal baseline study SHALL consume the same dataset-specific embeddings and ground truth artifacts.

#### Scenario: Embeddings are generated under a frozen encoder configuration
- **WHEN** document/base and query embeddings are generated for a dataset
- **THEN** the encoder configuration SHALL first be recorded in `07_ENCODER_REGISTRY_TEMPLATE.csv` or its instantiated registry
- **AND** the resulting embeddings SHALL be written under `/home/zcq/VDB/data/formal_baselines/<dataset>/embeddings/`

#### Scenario: ANN ground truth is generated for every primary dataset
- **WHEN** a primary dataset becomes runnable for baseline experiments
- **THEN** the pipeline SHALL generate `gt_top10.npy`, `gt_top20.npy`, and `gt_top100.npy` under `/home/zcq/VDB/baselines/data/formal_baselines/<dataset>/gt/`
- **AND** the metric used to generate ground truth SHALL match the retrieval metric used by the experiment

#### Scenario: Task labels are preserved for labeled datasets
- **WHEN** a dataset provides task labels in addition to ANN ground truth
- **THEN** those labels SHALL be stored under `/home/zcq/VDB/baselines/data/formal_baselines/<dataset>/gt/`
- **AND** `msmarco_passage` SHALL preserve `qrels.tsv` while `amazon_esci` SHALL preserve `esci_labels.parquet`

### Requirement: Payload backend exports SHALL stay schema-aligned across backends
Every dataset that participates in the formal baseline study SHALL export aligned payload representations for the selected storage backends.

#### Scenario: Standard payload backends are exported for semantic datasets
- **WHEN** `coco_100k`, `msmarco_passage`, or `amazon_esci` payloads are exported
- **THEN** the pipeline SHALL emit backend outputs for `FlatStor`, `Lance`, and `Parquet` under `/home/zcq/VDB/baselines/data/formal_baselines/<dataset>/`
- **AND** the exported backends SHALL preserve the same logical field set

#### Scenario: Deep8M payload sizes remain distinguishable across backends
- **WHEN** `deep8m_synth` payloads are exported
- **THEN** each payload size tier SHALL be exported as a separate backend output family
- **AND** the size tier identity SHALL remain reconstructable from the output path and registry metadata
