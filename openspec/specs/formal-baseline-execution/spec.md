## ADDED Requirements

### Requirement: Formal baseline execution SHALL cover the mandatory system suite
The formal baseline study SHALL define a mandatory system suite for the primary comparison and a gated optional suite for secondary analysis.

#### Scenario: Phase-1 baseline suite is frozen before execution
- **WHEN** Phase 0 of the study is completed
- **THEN** `baseline_registry.csv` SHALL include active phase-1 entries for `Faiss IVFPQ + rerank` and `Faiss IVFResidualQuantizer` or `Faiss IVFLocalSearchQuantizer`
- **AND** `DiskANN` SHALL be recorded as a conditional thin-wrapper integration that may become `blocked` without blocking phase-1 closure
- **AND** `IVFPQR` SHALL NOT be part of the phase-1 mandatory comparison suite
- **AND** if retained in the workspace, `IVFPQR` SHALL be marked as diagnostic-only rather than mandatory
- **AND** `BoundFetch` SHALL remain recorded as deferred until its later optimization phase is ready

#### Scenario: Optional systems remain additive
- **WHEN** `ConANN + Faiss` or `Extended-RaBitQ` are considered
- **THEN** they SHALL be marked as optional in the baseline registry
- **AND** their absence SHALL NOT block execution of the mandatory suite

### Requirement: The study SHALL execute through a fixed phase order
The formal baseline study SHALL be run as an ordered sequence so that data preparation, system execution, and result aggregation remain reproducible.

#### Scenario: Registries are frozen before any benchmark run
- **WHEN** the study is initialized
- **THEN** dataset, encoder, and baseline registries SHALL be instantiated before any formal run is started
- **AND** later benchmark runs SHALL reference those registries rather than ad hoc parameters

#### Scenario: Primary datasets run before appendix datasets
- **WHEN** execution batches are scheduled
- **THEN** the primary dataset order SHALL be `COCO 100K`, `MS MARCO Passage`, `Deep8M-synth`, and `Amazon ESCI`
- **AND** appendix datasets such as `LAION subset`, `Clotho`, and `MSR-VTT` SHALL remain gated until the primary suite is stable

#### Scenario: Baseline runs are grouped by output class
- **WHEN** a dataset enters the execution phase
- **THEN** the study SHALL produce vector-search runs, coupled E2E runs, payload microbench runs, and build/startup runs
- **AND** each output class SHALL use a stable output root under `/home/zcq/VDB/baselines/formal-study/outputs/`

### Requirement: The study SHALL distinguish main experiments from extended experiments
The formal baseline study SHALL split its execution into a primary experiment suite and an extended experiment suite so that result-scale analysis and backend analysis stay separable.

#### Scenario: Main experiments use the primary datasets and the primary backend
- **WHEN** the main experiment suite is scheduled
- **THEN** it SHALL run on `COCO 100K`, `MS MARCO Passage`, `Deep8M-synth`, and `Amazon ESCI`
- **AND** it SHALL use `FlatStor` as the primary backend
- **AND** it SHALL evaluate `topk ∈ {10, 50, 100}`

#### Scenario: Extended experiments reuse top-10 operating points for backend comparison
- **WHEN** the extended experiment suite is scheduled
- **THEN** it SHALL use only `topk=10`
- **AND** it SHALL select operating points from the main experiment's top-10 search results
- **AND** it SHALL replay those points on `FlatStor`, `Lance`, and `Parquet`

### Requirement: Main-experiment execution SHALL be summarized incrementally by dataset and top-k
The formal baseline study SHALL not defer all main-experiment organization until every primary dataset has completed. Instead, each completed `dataset × topk` slice SHALL be summarized immediately so that partial progress remains usable and auditable.

#### Scenario: A dataset top-k sweep finishes on the primary backend
- **WHEN** a main-experiment sweep for one `dataset × topk` combination has completed across the required active methods on the primary backend
- **THEN** the study SHALL immediately aggregate the corresponding vector-search and coupled E2E outputs for that same `dataset × topk`
- **AND** it SHALL generate a dataset-local Pareto-ready summary before scheduling later datasets or later global summaries

#### Scenario: A dataset finishes one top-k tier before the others
- **WHEN** one dataset has completed `topk=10` but its `topk=50` and `topk=100` runs are still pending
- **THEN** the completed `topk=10` slice SHALL already be eligible for summary generation and Pareto plotting
- **AND** the study SHALL NOT require `topk=50` and `topk=100` to finish before preserving the `topk=10` result for that dataset

### Requirement: Main experiments SHALL use a frozen operating-point policy
The formal baseline study SHALL freeze the primary search controls before main-experiment execution so that recall-latency trade-offs are comparable across systems and top-k regimes.

#### Scenario: Primary datasets use fixed phase-1 `nlist` values
- **WHEN** the main IVF baseline suite is scheduled
- **THEN** `COCO 100K` SHALL use `nlist=2048`
- **AND** `Deep8M-synth` SHALL use `nlist=12800`
- **AND** `MS MARCO Passage` SHALL use `nlist=16384`
- **AND** `Amazon ESCI` SHALL use `nlist=8192` unless a later dataset-cardinality review explicitly changes that value before its first formal run

#### Scenario: Rerank budget is frozen by `topk` tier for main IVF baselines
- **WHEN** `Faiss IVFPQ + rerank` or `Faiss IVF-RQ + rerank` is run in the main experiment suite
- **THEN** the study SHALL use:
  - `topk=10 -> candidate_budget=100`
  - `topk=50 -> candidate_budget=250`
  - `topk=100 -> candidate_budget=500`
- **AND** the runner SHALL NOT sweep rerank budget during the primary operating-point search

#### Scenario: Main sweeps vary only the probe-depth control
- **WHEN** `Faiss IVFPQ + rerank` or `Faiss IVF-RQ + rerank` is explored for its primary recall-latency curve
- **THEN** the study SHALL sweep `nprobe`
- **AND** it SHALL NOT treat `nlist` as a primary sweep axis during phase 1
- **AND** if `DiskANN` is active, its primary search sweep SHALL vary `L_search` rather than introducing additional graph-build sweeps

### Requirement: Extended backend comparison SHALL use recall-matched top-10 operating points
The formal baseline study SHALL compare `FlatStor`, `Lance`, and `Parquet` using `topk=10` operating points that are aligned by achieved `recall@10` rather than by raw search parameter identity.

#### Scenario: Backend runs are selected from top-10 recall targets
- **WHEN** a `dataset × method` main-experiment search sweep has completed on the primary backend for `topk=10`
- **THEN** the study SHALL select operating points whose `recall@10` is closest to the target levels `0.80`, `0.90`, and `0.95`
- **AND** those selected operating points SHALL be the ones replayed on `FlatStor`, `Lance`, and `Parquet`

#### Scenario: Unreached top-10 recall targets remain explicit
- **WHEN** a method cannot reach one of the target `recall@10` levels on a dataset
- **THEN** the study SHALL record that target as `unreached` or `best-effort`
- **AND** it SHALL NOT substitute an arbitrary parameter-matched run in place of the missing recall-matched point

### Requirement: Formal E2E runs SHALL use the coupled search-plus-payload protocol
The primary benchmark protocol for the formal baseline study SHALL measure a single query path that includes both vector retrieval and payload fetch.

#### Scenario: Coupled timing starts at query submission
- **WHEN** a formal E2E run is executed
- **THEN** timing SHALL begin when the query vector is submitted into the system
- **AND** the measured interval SHALL include coarse search, probing, rerank, payload fetch, and required decode or assembly work

#### Scenario: Coupled timing ends after payload completion
- **WHEN** a query has identified the final records that must be fetched
- **THEN** the system SHALL trigger payload reads immediately
- **AND** the E2E timing window SHALL end only after the required payload fetch for that query completes

#### Scenario: Payload microbench does not replace formal E2E
- **WHEN** payload backend microbenchmarks are run
- **THEN** they SHALL be recorded as diagnostic results
- **AND** they SHALL NOT replace the coupled E2E result in the primary comparison

### Requirement: The study SHALL emit a common metric contract across systems
Every mandatory system in the formal baseline study SHALL export the same minimum metric families so that comparison, Pareto selection, and narrative analysis can be performed without system-specific parsing.

#### Scenario: Vector-search outputs share a metric schema
- **WHEN** a vector-search run completes
- **THEN** it SHALL emit system identity, dataset identity, control parameters, recall metrics, latency metrics, and QPS
- **AND** the exported fields SHALL be sufficient to reconstruct a recall-latency Pareto curve for that system family

#### Scenario: Formal E2E outputs share a metric schema
- **WHEN** a coupled E2E run completes
- **THEN** it SHALL emit `ann_core_ms`, `payload_fetch_ms`, `bytes_read`, `fetch_count`, `e2e_ms`, and percentile latency fields
- **AND** those fields SHALL be available for every active phase-1 baseline that participates in the main or extended comparison
- **AND** the exported metrics SHALL be sufficient to compute a top-k-matched recall metric for `topk=10`, `50`, and `100`

#### Scenario: Build and startup costs are captured separately
- **WHEN** a system is prepared for formal comparison
- **THEN** the study SHALL record `build_time`, `peak_rss`, `index_bytes`, `open_time`, and preload-related metrics where applicable
- **AND** those values SHALL be emitted into the build/startup output family rather than mixed into per-query metrics
