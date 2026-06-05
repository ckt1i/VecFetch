## ADDED Requirements

### Requirement: Recall@100 refreshed summaries SHALL expose only the active main top-k tiers
The refreshed ICDE baseline aggregation for this change SHALL publish `topk=10` and `topk=100` as the active main result tiers and SHALL keep old `topk=50` artifacts out of the refreshed main summary.

#### Scenario: Aggregation filters old top-k tiers from refreshed outputs
- **WHEN** the recall@100 aggregation stage updates `icde_baseline_summary.csv` and `icde_baseline_selected.csv`
- **THEN** those refreshed files SHALL contain rows only for `topk=10` and `topk=100`
- **AND** they SHALL NOT contain rows for `topk=20` or `topk=50`

#### Scenario: Prior topk fifty artifacts remain auditable
- **WHEN** the refreshed aggregation excludes `topk=50` from the main output surface
- **THEN** existing `topk=50` metrics files, probe files, backups, and historical outputs SHALL remain on disk
- **AND** the aggregation SHALL NOT delete those prior artifacts

#### Scenario: Recall one hundred is part of the common metric surface
- **WHEN** vector-search, coupled E2E, or DiskANN rows are written for the refreshed rerun
- **THEN** each row SHALL include `recall_at_topk`
- **AND** each row SHALL include `recall@10`
- **AND** each row SHALL include `recall@100`

#### Scenario: Refreshed summary preserves selected index identities
- **WHEN** DiskANN rows are aggregated into the refreshed ICDE summary
- **THEN** each dataset SHALL have exactly one non-empty DiskANN `index_id` in the refreshed result surface
- **AND** `amazon_esci` SHALL use only `diskann_disk_amazon_esci_R64_L100_PQ0`

### Requirement: Recall@100 selection SHALL record matched and best-effort outcomes
The refreshed selected-result table SHALL continue threshold-matched selection, but it SHALL not require every group to reach the highest recall@100 threshold.

#### Scenario: Topk one hundred threshold selection uses three targets
- **WHEN** selected rows are generated for `topk=100`
- **THEN** the aggregator SHALL evaluate target recalls `0.80`, `0.90`, and `0.95`
- **AND** it SHALL select the fastest qualified row for each reached target

#### Scenario: Unreached recall one hundred targets become best-effort rows
- **WHEN** a `dataset × system × backend × topk=100` group does not reach a target recall
- **THEN** the selected table SHALL still emit a row for that target
- **AND** the row SHALL be marked `best-effort`
- **AND** the selected row SHALL be the valid candidate with the highest achieved `recall_at_topk`

#### Scenario: Report explicitly states no coverage guarantee
- **WHEN** the Markdown report summarizes `topk=100` results
- **THEN** it SHALL report which threshold targets were matched and which were best-effort
- **AND** it SHALL NOT describe `Recall@100 >= 0.95` as a required pass condition for this change

#### Scenario: Aggregation creates backups before replacing shared outputs
- **WHEN** the refreshed summary, selected table, or report is replaced
- **THEN** the previous files SHALL be copied into the ICDE baseline backups directory first
- **AND** the final report SHALL remain traceable to the source paths used for aggregation
