## ADDED Requirements

### Requirement: Formal baseline manifests and trackers SHALL be instantiated in a dedicated workspace
The formal baseline study SHALL create and maintain its own manifests and trackers under `/home/zcq/VDB/baselines/formal-study/`.

#### Scenario: Core manifests are created from the frozen templates
- **WHEN** the formal baseline study workspace is initialized
- **THEN** it SHALL create dataset, encoder, and baseline registries from the corresponding templates in `refine-logs/formal-baseline-study-2026-04-16/`
- **AND** those instantiated registries SHALL live under `/home/zcq/VDB/baselines/formal-study/manifests/`

#### Scenario: Run status and failure tracking are always available
- **WHEN** a run is scheduled or executed
- **THEN** the workspace SHALL provide `RUN_STATUS.csv` and `FAILURES.md` under `/home/zcq/VDB/baselines/formal-study/trackers/`
- **AND** every run SHALL be traceable by `run_id`, phase, dataset, system, protocol, status, and output directory

### Requirement: Per-run outputs SHALL use canonical output roots
The formal baseline study SHALL write each run into a canonical directory tree so that later aggregation is deterministic.

#### Scenario: Vector-search outputs use the vector-search root
- **WHEN** a vector-search run completes
- **THEN** its `metrics.csv` SHALL be written to `outputs/vector_search/{dataset}/{system}/{run_id}/metrics.csv`
- **AND** the run metadata SHALL be reconstructable from the path and tracker entry

#### Scenario: Coupled E2E outputs use the E2E root
- **WHEN** a formal E2E run completes
- **THEN** its `metrics.csv` SHALL be written to `outputs/e2e/{dataset}/{system}/{run_id}/metrics.csv`
- **AND** a dataset-level summary file SHALL be generatable at `outputs/e2e/{dataset}/e2e_system_summary.csv`

#### Scenario: Main and extended experiment summaries remain distinguishable
- **WHEN** the study aggregates formal baseline outputs
- **THEN** it SHALL preserve distinct summaries for the main experiment suite and the extended experiment suite
- **AND** the aggregation SHALL make the originating `topk` tier and experiment type reconstructable from the summary rows

#### Scenario: Payload microbench and build-cost outputs stay separated
- **WHEN** payload microbench or build/startup runs complete
- **THEN** they SHALL be written to `outputs/payload_microbench/...` and `outputs/build_cost/...` respectively
- **AND** they SHALL NOT overwrite or masquerade as the primary E2E result

### Requirement: Aggregation SHALL produce study-level summaries and controlled backfill
The formal baseline study SHALL aggregate per-run outputs into study-level summaries before any shared results directory is updated.

#### Scenario: Formal-study aggregation happens before public backfill
- **WHEN** a batch of runs has completed and passed sanity checks
- **THEN** the aggregator SHALL first update summary CSVs and plots under `/home/zcq/VDB/baselines/formal-study/outputs/`
- **AND** only after that step may selected results be backfilled into `/home/zcq/VDB/baselines/results/`

#### Scenario: Dataset-local top-k summaries are emitted as soon as a slice completes
- **WHEN** a single `dataset × topk` slice of the main experiment has finished
- **THEN** the aggregator SHALL immediately write the corresponding dataset-local summary CSVs under `/home/zcq/VDB/baselines/formal-study/outputs/`
- **AND** it SHALL do so without waiting for the remaining datasets in the main suite to complete

#### Scenario: Dataset-local Pareto plots are emitted per top-k slice
- **WHEN** a single `dataset × topk` slice of the main experiment has finished and its summary CSV is available
- **THEN** the aggregator SHALL generate a Pareto plot for that same `dataset × topk` slice
- **AND** the plot SHALL be stored alongside the formal-study outputs so it can be reviewed before the full main-suite aggregation finishes

#### Scenario: Narrative analysis updates target stable destinations
- **WHEN** formal-study results are promoted beyond the workspace
- **THEN** the aggregator SHALL update the intended narrative destinations such as `analysis.md` or other study-facing summaries
- **AND** the source run ids used for the promotion SHALL remain recorded in the formal-study tracker

#### Scenario: Recall-matched backend summaries preserve target metadata
- **WHEN** a storage-backend comparison is aggregated for a `dataset × method`
- **THEN** the summary rows SHALL record the target `recall@10` level used for operating-point selection
- **AND** they SHALL record the selected control parameter and the actual achieved `recall@10`
- **AND** if a target was not reached, the summary SHALL preserve an explicit `unreached` or `best-effort` marker rather than silently omitting the row

### Requirement: Partial runs and retries SHALL remain auditable
The formal baseline study SHALL preserve enough tracker information to diagnose partial failures, reruns, and missing outputs.

#### Scenario: A failed run is recorded without hiding the failure
- **WHEN** a benchmark, export, or build step fails
- **THEN** the study SHALL append an entry to `FAILURES.md` with the dataset, system, configuration, and log path
- **AND** the corresponding `RUN_STATUS.csv` row SHALL remain visible as failed or blocked until a later retry succeeds

#### Scenario: A rerun keeps the previous history
- **WHEN** a failed or superseded run is retried
- **THEN** the retried run SHALL receive a distinct `run_id`
- **AND** the earlier run record SHALL remain in the tracker for auditability
