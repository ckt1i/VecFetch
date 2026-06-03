## ADDED Requirements

### Requirement: Recall@100 ICDE baseline rerun SHALL cover the current runnable baseline matrix
The formal ICDE baseline suite SHALL provide an execution mode for the refreshed `Recall@10` / `Recall@100` experiment surface across the currently runnable formal baseline datasets and baseline methods.

#### Scenario: Rerun dataset scope is fixed to current ICDE-ready datasets
- **WHEN** the recall@100 baseline rerun is scheduled
- **THEN** the suite SHALL include `coco_100k`, `msmarco_passage`, `amazon_esci`, `imagenet1k`, and `voxceleb2_ecapa_150k`
- **AND** it SHALL NOT include `deep8m_synth`, `clotho`, `msrvtt`, or any other dataset that is not phase0-ready for the current ICDE suite

#### Scenario: Rerun top-k tiers are configurable
- **WHEN** an operator invokes the ICDE baseline suite
- **THEN** the suite SHALL allow selecting the top-k tiers to execute
- **AND** the refreshed ICDE rerun SHALL support running only `topk=100` for execution stages
- **AND** the refreshed aggregation SHALL support `topk=10` and `topk=100` together

#### Scenario: All six baseline combinations are scheduled for topk one hundred
- **WHEN** a dataset enters the recall@100 rerun
- **THEN** the suite SHALL schedule `IVF+PQ+FlatStor`, `IVF+PQ+Lance`, `IVF+RQ+FlatStor`, `IVF+RQ+Lance`, `DiskANN+FlatStor`, and `DiskANN+Lance`
- **AND** each combination SHALL use `topk=100`

#### Scenario: IVF rerun uses the current fixed-budget policy
- **WHEN** `IVF+PQ` or `IVF+RQ` is run for `topk=100`
- **THEN** the suite SHALL use `candidate_budget=2000`
- **AND** it SHALL reuse existing FAISS, RaBitQ, and vector-search caches when the dataset, top-k, and search parameters match

### Requirement: DiskANN recall@100 rerun SHALL use existing indexes only
The DiskANN portion of the recall@100 rerun SHALL use only the existing accepted raw-vector DiskANN index directories and SHALL NOT build new DiskANN graph indexes.

#### Scenario: DiskANN uses one existing index per dataset
- **WHEN** DiskANN is scheduled for the refreshed rerun
- **THEN** `coco_100k` SHALL use `/home/zcq/VDB/baselines/data/diskann_disk_coco_100k_R64_L100_PQ0`
- **AND** `msmarco_passage` SHALL use `/home/zcq/VDB/baselines/data/diskann_disk_msmarco_passage_R32_L50_PQ0`
- **AND** `amazon_esci` SHALL use `/home/zcq/VDB/baselines/data/diskann_disk_amazon_esci_R64_L100_PQ0`
- **AND** `imagenet1k` SHALL use `/home/zcq/VDB/baselines/data/diskann_disk_imagenet1k_R13_L100_PQ0`
- **AND** `voxceleb2_ecapa_150k` SHALL use `/home/zcq/VDB/baselines/data/diskann_disk_voxceleb2_ecapa_150k_R13_L100_PQ0`

#### Scenario: DiskANN runner fails before build on missing index
- **WHEN** a required DiskANN index directory is missing, empty, or has a manifest that does not match the requested dataset, `R`, `L_build`, and `pq_disk_bytes=0`
- **THEN** the suite SHALL stop that DiskANN run before invoking `build_disk_index`
- **AND** it SHALL record the missing or mismatched index as a blocking condition rather than creating a replacement index

#### Scenario: DiskANN command line carries the accepted index identity
- **WHEN** a DiskANN recall@100 command is constructed
- **THEN** it SHALL pass the explicit `--index-dir` for the accepted index
- **AND** it SHALL pass the matching `--R`, `--L`, and `--pq-disk-bytes 0` values
- **AND** it SHALL NOT use a default index path derived only from dataset, `R`, and `L`

#### Scenario: DiskANN search is best-effort under the no-build constraint
- **WHEN** an existing DiskANN index cannot reach `Recall@100 >= 0.95`
- **THEN** the run SHALL still be eligible for aggregation as a best-effort result
- **AND** the suite SHALL NOT build a stronger index unless a later change explicitly authorizes rebuilding
