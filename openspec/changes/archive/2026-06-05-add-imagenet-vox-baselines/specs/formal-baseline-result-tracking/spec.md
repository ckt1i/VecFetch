## ADDED Requirements

### Requirement: Expansion baseline tracking SHALL separate probes from promoted results
The formal-study trackers and summaries SHALL keep ImageNet/Vox probe outputs separate from main ICDE baseline summaries until validation and promotion have completed.

#### Scenario: Probe outputs are written under a separate location
- **WHEN** DiskANN search grids, coverage probes, or readiness probes are run for `imagenet1k` or `voxceleb2_ecapa_150k`
- **THEN** their CSVs and logs SHALL be written under a probe-specific output path
- **AND** the main ICDE baseline summary SHALL remain unchanged until promotion is explicitly performed

#### Scenario: Promotion backs up any replaced summary
- **WHEN** accepted expansion baseline rows are promoted into an existing ICDE baseline CSV
- **THEN** the previous CSV SHALL be copied to a timestamped backup first
- **AND** the promotion SHALL replace only rows for the same dataset/system/backend/top-k scope
- **AND** unrelated dataset rows SHALL remain unchanged

### Requirement: Expansion baseline aggregation SHALL enforce no-Parquet and no-Recall20 invariants
The aggregation path for `imagenet1k` and `voxceleb2_ecapa_150k` SHALL reject rows that violate the requested backend or metric scope.

#### Scenario: Parquet rows are excluded from expansion summaries
- **WHEN** the ICDE baseline summary is generated for expansion datasets
- **THEN** rows with backend `parquet` or `payload_parquet` SHALL be excluded
- **AND** the generated report SHALL state that the required payload backend set is `flatstor,lance`

#### Scenario: Recall20 rows are excluded from expansion summaries
- **WHEN** the ICDE baseline summary or selected-point table is generated for expansion datasets
- **THEN** rows for `topk=20` or fields promoted as `Recall@20` SHALL be excluded
- **AND** the report SHALL preserve only `Recall@10`, `Recall@50`, and `recall_at_topk` fields needed for those top-k tiers

### Requirement: DiskANN coverage decisions SHALL be auditable in the promoted report
The final report for expansion baseline rows SHALL preserve enough metadata to explain why a DiskANN index was accepted or rejected.

#### Scenario: Accepted DiskANN index records coverage metadata
- **WHEN** a DiskANN index is promoted for an expansion dataset
- **THEN** the report SHALL record the accepted index id, manifest identity, graph build parameters, search grid, IVF+RaBitQ recall range, DiskANN recall range, and any operator high-recall cap used for acceptance
- **AND** all promoted DiskANN rows for that dataset SHALL reference the accepted index id

#### Scenario: Rejected DiskANN probes remain traceable
- **WHEN** a DiskANN candidate index is rejected because it fails the coverage gate or produces invalid results
- **THEN** the failure reason SHALL remain in the probe CSV, run log, or report
- **AND** rejected probe rows SHALL NOT be silently merged into the main summary

#### Scenario: Single-index invariant is validated after promotion
- **WHEN** aggregation completes for an expansion dataset
- **THEN** each promoted `dataset × DiskANN` group SHALL contain exactly one non-empty DiskANN index id
- **AND** the validator SHALL fail or mark the summary blocked if multiple promoted DiskANN index ids are present for the same dataset
