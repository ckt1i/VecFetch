## ADDED Requirements

### Requirement: ICDE expansion baseline execution SHALL run only selected datasets and required payload backends
The ICDE baseline runner SHALL support executing `imagenet1k` and `voxceleb2_ecapa_150k` without forcing a full rerun of unrelated datasets or unsupported payload backends.

#### Scenario: Runner accepts an explicit dataset subset
- **WHEN** the operator requests an ICDE baseline run for `imagenet1k`, `voxceleb2_ecapa_150k`, or both
- **THEN** the runner SHALL schedule only the requested dataset set
- **AND** it SHALL NOT rerun COCO100K, MS MARCO, Amazon ESCI, or other datasets unless they are explicitly requested

#### Scenario: Expansion baselines use six required system/backend combinations
- **WHEN** an expansion dataset baseline sweep is scheduled
- **THEN** the runner SHALL include `IVF+PQ+FlatStor`, `IVF+PQ+Lance`, `IVF+RaBitQ+FlatStor`, `IVF+RaBitQ+Lance`, `DiskANN+FlatStor`, and `DiskANN+Lance`
- **AND** matching FlatStor and Lance rows for the same search family SHALL use the same ANN search parameters and index identity where applicable

#### Scenario: Parquet backend is excluded from execution
- **WHEN** an expansion dataset baseline sweep is scheduled
- **THEN** the runner SHALL NOT schedule Parquet payload-backend E2E runs
- **AND** it SHALL NOT require Parquet payload-backend smoke tests
- **AND** it SHALL NOT aggregate Parquet rows for `imagenet1k` or `voxceleb2_ecapa_150k`

#### Scenario: Expansion baseline metrics are limited to K10 and K50
- **WHEN** an expansion dataset baseline sweep is scheduled or aggregated
- **THEN** it SHALL include only `topk=10` and `topk=50`
- **AND** it SHALL NOT schedule, compute, or promote `Recall@20` rows

### Requirement: DiskANN expansion baselines SHALL promote one index per dataset after IVF+RaBitQ recall coverage passes
DiskANN probing SHALL keep any candidate graph build configurations isolated, and the promoted ICDE baseline rows SHALL use only one accepted DiskANN index per expansion dataset.

#### Scenario: IVF+RaBitQ reference range is collected before DiskANN promotion
- **WHEN** IVF+RaBitQ runs have completed for an expansion dataset and top-k tier
- **THEN** the runner or promotion script SHALL compute the minimum and maximum valid IVF+RaBitQ recall values for that dataset and top-k tier
- **AND** those reference values SHALL be recorded with the DiskANN coverage check

#### Scenario: DiskANN probe rows remain isolated before coverage passes
- **WHEN** DiskANN candidate indexes or search grids are being evaluated
- **THEN** their rows SHALL be written to a probe output separate from the main ICDE baseline table
- **AND** those rows SHALL NOT appear in the promoted summary until the accepted index passes the coverage gate

#### Scenario: One DiskANN index passes recall coverage
- **WHEN** a candidate DiskANN index has a valid `L_search` and beam grid
- **THEN** the lowest valid DiskANN recall point for each required top-k tier SHALL be at or below the corresponding lowest valid IVF+RaBitQ recall point
- **AND** the highest valid DiskANN recall point for each required top-k tier SHALL be at or above the corresponding highest valid IVF+RaBitQ recall point, unless an operator high-recall cap is explicitly requested
- **AND** when an operator high-recall cap is requested, the high endpoint SHALL be evaluated against `min(highest valid IVF+RaBitQ point, operator high-recall cap)`
- **AND** the final promoted rows for that dataset SHALL reference only that accepted index directory and manifest identity

#### Scenario: DiskANN coverage does not pass on the first search grid
- **WHEN** a candidate DiskANN index fails the low-recall side of the coverage gate
- **THEN** the search grid SHALL be expanded toward lower `L_search` or beam settings before building another index
- **AND** the failed probe rows SHALL remain auditable but unpromoted

#### Scenario: DiskANN high-recall side cannot be recovered by search parameters
- **WHEN** a candidate DiskANN index fails the high-recall side after increasing `L_search` and beam within the configured probe budget
- **THEN** the implementation SHALL build a stronger graph index before promotion can proceed
- **AND** only the new index SHALL be eligible for final promotion if it passes the coverage gate

### Requirement: Expansion baselines SHALL reuse existing indexes and vector-search outputs whenever compatible
The baseline execution path SHALL avoid rebuilding ANN artifacts when an existing cache or index matches the requested dataset, metric, dimensions, row count, and control parameters.

#### Scenario: IVF search output is shared across payload backends
- **WHEN** IVF+PQ or IVF+RaBitQ vector-search results exist for an expansion dataset, top-k tier, and parameter setting
- **THEN** FlatStor and Lance E2E replay SHALL reuse those search outputs rather than rerunning the ANN search

#### Scenario: DiskANN index identity is reused across payload backends
- **WHEN** DiskANN+FlatStor and DiskANN+Lance are promoted for the same expansion dataset
- **THEN** both backend rows SHALL reference the same DiskANN index directory and manifest identity
- **AND** only payload-fetch timing and payload-byte fields SHALL differ because of the backend
