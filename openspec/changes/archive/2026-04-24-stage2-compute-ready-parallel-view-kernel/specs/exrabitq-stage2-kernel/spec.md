## MODIFIED Requirements

### Requirement: Two-Stage ExRaBitQ Optimization Path
The system SHALL treat `IPExRaBitQ` optimization as a staged capability. The earlier layout-compatible kernel experiments and the first resident parallel-view experiment SHALL be treated as prior validation, and the next stage SHALL upgrade the resident view to a compute-ready representation together with a lane-batched query-time kernel. The system SHALL NOT consider the optimization complete if the resident view is upgraded but the query-time kernel remains lane-local.

#### Scenario: Compute-ready view is required for the new stage
- **WHEN** the system enters the next resident-view optimization stage
- **THEN** it SHALL build query-independent compute-ready resident metadata rather than only a slice-friendly address reordering

#### Scenario: Kernel change is mandatory together with the view
- **WHEN** the compute-ready resident view is enabled
- **THEN** the Stage2 query-time kernel SHALL consume it in lane batches instead of retaining a lane-local main loop

### Requirement: Stage 1 Kernel Must Be Cross-Dimension and AVX-512 First
The Stage 1 and later Stage2-serving kernels SHALL target `AVX-512` first and SHALL remain organized as cross-dimension block kernels rather than single-dimension-specialized implementations. The compute-ready resident path SHALL preserve correctness across the same common-dimension set while enabling lane-batched query-time execution.

#### Scenario: Compute-ready path remains cross-dimension
- **WHEN** the compute-ready resident path is used on dimensions such as 96, 128, 256, 384, 512, 768, 1024, or 1536
- **THEN** the system SHALL process them through the same block-and-slice hierarchy rather than introducing a fixed-dimension-only serving path

#### Scenario: Tail dimensions remain correct on compute-ready path
- **WHEN** a block contains a tail that is not a full preferred slice
- **THEN** the compute-ready resident path SHALL preserve mathematical correctness for Stage2 scoring

### Requirement: Stage 1 Kernel Shall Avoid Signed-Float Code Reconstruction
The Stage2 query-time kernel SHALL avoid reconstructing a persistent full signed float code vector. Instead, the system SHALL split query-time work into batch sign preparation, query-dependent sign context instantiation, and batch abs consumption.

#### Scenario: Query-dependent sign context is ephemeral
- **WHEN** the Stage2 kernel evaluates a lane batch
- **THEN** it SHALL create a query-time sign context from resident sign metadata and the current query slice, and SHALL NOT materialize a persistent signed-float code vector in resident storage

#### Scenario: Bias term remains mathematically equivalent
- **WHEN** the Stage2 kernel separates sign preparation, sign application, and abs consumption
- **THEN** the resulting `ip_raw` SHALL remain mathematically equivalent to the reference `IPExRaBitQ` result

## ADDED Requirements

### Requirement: Compute-Ready Resident Sign Pack
The resident Stage2 view SHALL provide a query-independent `sign_pack` representation for each batch slice. `sign_pack` SHALL be batch-friendly and SHALL contain enough information to derive a query-time sign context for a lane batch without reinterpreting raw packed sign bits lane-by-lane inside the hot loop.

#### Scenario: Resident sign pack contains lane and bitplane information
- **WHEN** resident Stage2 metadata is built for a batch slice
- **THEN** the resident sign representation SHALL include per-lane sign words and a batch-friendly sign distribution view sufficient for query-time batch sign preparation

#### Scenario: Sign pack remains query-independent
- **WHEN** resident Stage2 metadata is built before any query is known
- **THEN** the sign pack SHALL contain only query-independent information and SHALL NOT precompute any signed query values

### Requirement: Compute-Ready Resident Abs Pack
The resident Stage2 view SHALL provide a query-independent `abs_pack` representation for each batch slice. `abs_pack` SHALL be organized for lane-batched query-time consumption rather than only for lane-local address convenience.

#### Scenario: Resident abs pack is batch-consumable
- **WHEN** resident Stage2 metadata is built for a batch slice
- **THEN** the resident abs representation SHALL expose per-slice batch-consumable magnitude data for the lane batch

#### Scenario: Abs pack remains query-independent
- **WHEN** resident Stage2 metadata is built before any query is known
- **THEN** the abs pack SHALL contain only query-independent magnitude metadata and SHALL NOT precompute query-dependent dot-product state

### Requirement: Query-Time Sign Context Separation
The query-time kernel SHALL derive a lightweight `sign_ctx` from `sign_pack` and the current query slice before entering batch abs consumption. `sign_ctx` SHALL be an ephemeral batch-local state and SHALL be separate from resident metadata.

#### Scenario: Sign context is built from sign pack and query slice
- **WHEN** the query-time kernel starts processing a batch slice
- **THEN** it SHALL derive the sign context from the resident sign pack and the current query slice before batch sign application

#### Scenario: Sign context is not stored in resident metadata
- **WHEN** the system builds resident metadata
- **THEN** it SHALL NOT persist the query-time sign context or any signed-query result in the resident view

### Requirement: Lane-Batched Query-Time Kernel
The compute-ready Stage2 query-time kernel SHALL process lanes in batches. It SHALL organize the hot path as batch sign preparation, batch sign application, and batch abs consumption; merely replacing address layout while retaining lane-by-lane execution SHALL NOT satisfy this requirement.

#### Scenario: Batch phases are explicit
- **WHEN** the compute-ready Stage2 kernel evaluates a batch slice
- **THEN** it SHALL execute explicit batch-oriented sign preparation, sign application, and abs consumption phases

#### Scenario: Lane-local fallback is not the primary compute-ready path
- **WHEN** the compute-ready resident view is available
- **THEN** the primary query-time kernel SHALL operate on lane batches rather than iterating each lane independently through the main sign and abs path

### Requirement: Resident Build and Query-Time Costs Remain Observable
The system SHALL continue to expose the cost of building and using the compute-ready resident view so that the resident-path tradeoff remains measurable.

#### Scenario: Resident build cost is observable
- **WHEN** the compute-ready resident metadata is built
- **THEN** benchmark output SHALL report build-time and resident-memory related metrics for that metadata

#### Scenario: Query-time kernel attribution remains observable
- **WHEN** fine-grained timing is enabled
- **THEN** benchmark output SHALL continue to report Stage2 kernel sub-attribution sufficient to judge whether sign handling and abs consumption improved
