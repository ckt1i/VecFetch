## MODIFIED Requirements

### Requirement: Formal baseline execution SHALL cover the mandatory system suite
The formal baseline study SHALL define a mandatory system suite for the ICDE baseline comparison and a gated optional suite for secondary analysis.

#### Scenario: ICDE baseline suite is frozen before execution
- **WHEN** the ICDE baseline execution begins
- **THEN** `baseline_registry.csv` SHALL include active or schedulable entries for `IVF+PQ`, `IVF+RQ/RaBitQ`, and `DiskANN`
- **AND** each mandatory search family SHALL be runnable with the payload-store combinations required by the ICDE plan
- **AND** `BoundFetch` SHALL remain outside the baseline-only execution wave
- **AND** `IVFPQR`, `ConANN`, `Extended-RaBitQ`, and Parquet backend runs SHALL NOT block completion of the ICDE baseline matrix

#### Scenario: Optional systems remain additive
- **WHEN** optional systems or additional backends are considered
- **THEN** they SHALL be marked as optional or diagnostic
- **AND** their absence SHALL NOT block execution of the mandatory ICDE baseline suite

### Requirement: The study SHALL distinguish main experiments from extended experiments
The formal baseline study SHALL split its execution into an ICDE baseline suite and any extended or historical experiment suites so that the current paper baseline matrix is not mixed with stale top-k or backend settings.

#### Scenario: ICDE baseline experiments use the required datasets and payload stores
- **WHEN** the ICDE baseline suite is scheduled
- **THEN** it SHALL run on `COCO 100K`, `MS MARCO Passage`, and `Amazon ESCI`
- **AND** it SHALL evaluate `topk ∈ {10, 50}`
- **AND** it SHALL run the following required baseline combinations when their runner support is available:
  - `IVF+PQ+FlatStor`
  - `IVF+PQ+Lance`
  - `IVF+RQ+FlatStor`
  - `IVF+RQ+Lance`
  - `DiskANN+FlatStor`
  - `DiskANN+Lance`

#### Scenario: Historical top-k tiers are excluded from the ICDE baseline suite
- **WHEN** an ICDE baseline execution plan is generated
- **THEN** it SHALL NOT schedule `topk=20` or `topk=100`
- **AND** any existing results for those tiers SHALL remain historical or appendix-only artifacts rather than active ICDE baseline rows

### Requirement: Main experiments SHALL use a frozen operating-point policy
The formal baseline study SHALL freeze the primary search controls before ICDE baseline execution so that recall-latency trade-offs are comparable across systems, payload stores, and top-k regimes.

#### Scenario: Primary datasets use fixed `nlist` values
- **WHEN** the ICDE IVF baseline suite is scheduled
- **THEN** `COCO 100K` SHALL use `nlist=2048`
- **AND** `MS MARCO Passage` SHALL use `nlist=16384`
- **AND** `Amazon ESCI` SHALL use `nlist=8192` unless a later dataset-cardinality review explicitly changes that value before its first ICDE run

#### Scenario: Rerank budget is frozen by `topk` tier for IVF baselines
- **WHEN** `IVF+PQ` or `IVF+RQ/RaBitQ` is run in the ICDE baseline suite
- **THEN** the study SHALL use:
  - `topk=10 -> candidate_budget=topk*20`
  - `topk=50 -> candidate_budget=topk*20`
- **AND** the runner SHALL NOT sweep rerank budget during the primary operating-point search

#### Scenario: Search controls are shared across payload stores
- **WHEN** a search family is run with both FlatStor and Lance
- **THEN** the FlatStor and Lance runs SHALL use the same dataset, top-k, index identity, and search parameters
- **AND** only payload-store timing and payload-store read metrics SHALL differ because of backend selection

#### Scenario: Indexes are reused before rebuilding
- **WHEN** a baseline run requests an index whose cache or manifest already matches dataset, metric, row count, dimensionality, and build parameters
- **THEN** the runner SHALL reuse that index
- **AND** the run metadata SHALL record the reused index identity
- **AND** the runner SHALL build a new index only when no compatible index exists or when a missing recall region requires a deliberately different build configuration

### Requirement: Formal E2E runs SHALL use the coupled search-plus-payload protocol
The primary benchmark protocol for the formal baseline study SHALL measure a single query path that includes both vector retrieval and payload fetch.

#### Scenario: Coupled timing starts at query submission
- **WHEN** a formal E2E run is executed
- **THEN** timing SHALL begin when the query vector is submitted into the system
- **AND** the measured interval SHALL include coarse search, probing or graph traversal, rerank when applicable, payload fetch, and required decode or assembly work

#### Scenario: Coupled timing ends after payload completion
- **WHEN** a query has identified the final records that must be fetched
- **THEN** the system SHALL trigger payload reads immediately through the selected backend
- **AND** the E2E timing window SHALL end only after the required payload fetch for that query completes

#### Scenario: DiskANN coupled timing uses the C++ CLI search path
- **WHEN** a DiskANN ICDE baseline run is executed
- **THEN** DiskANN search SHALL be performed through the C++ CLI path
- **AND** payload fetch SHALL be measured separately through FlatStor or Lance according to the requested backend
- **AND** the reported E2E latency SHALL combine DiskANN search latency and payload fetch latency under the same query set

### Requirement: The study SHALL emit a common metric contract across systems
Every mandatory system in the formal baseline study SHALL export the same minimum metric families so that comparison, Pareto selection, and narrative analysis can be performed without system-specific parsing.

#### Scenario: Formal E2E outputs share an ICDE metric schema
- **WHEN** an ICDE baseline run completes
- **THEN** it SHALL emit system identity, dataset identity, payload backend, top-k, search parameters, index identity, index reuse/build status, recall metrics, latency metrics, payload bytes read, and payload fetch count
- **AND** the exported metrics SHALL be sufficient to compute recall-matched fastest points for `Recall@10` and `Recall@50`

#### Scenario: DiskANN invalid points are rejected explicitly
- **WHEN** a DiskANN run produces duplicate IDs, incomplete top-k rows, query/ground-truth mismatch, payload misses, CLI parse failures, or timeout failures
- **THEN** the row SHALL be marked invalid with a reason
- **AND** invalid rows SHALL NOT enter Pareto frontier or threshold selection

#### Scenario: ICDE summaries preserve backend and reuse metadata
- **WHEN** the ICDE baseline summary is generated
- **THEN** each selected row SHALL include `dataset`, `system`, `backend`, `topk`, `recall_at_topk`, latency percentiles, bytes read, fetch count, selected parameters, and index reuse/build status
- **AND** FlatStor and Lance rows SHALL remain distinguishable in the summary
