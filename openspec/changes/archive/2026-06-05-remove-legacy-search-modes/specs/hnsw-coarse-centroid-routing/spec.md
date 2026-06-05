## ADDED Requirements

### Requirement: HNSW coarse routing SHALL NOT be a formal query backend
The system SHALL NOT expose HNSW over IVF centroids as a formal coarse routing backend. Formal query execution SHALL use exact centroid scoring or the supported two-level coarse routing path.

#### Scenario: HNSW routing option is removed
- **WHEN** a user runs the formal benchmark or query path
- **THEN** the system SHALL NOT offer `--hnsw-coarse-routing`, `--hnsw-coarse-m`, `--hnsw-coarse-ef-construction`, or `--hnsw-coarse-ef-search` as active tuning knobs
- **AND** HNSW routing SHALL NOT affect `FindNearestClusters()`

#### Scenario: Benchmark output omits HNSW routing fields
- **WHEN** benchmark results are exported
- **THEN** the output SHALL NOT include HNSW routing configuration or HNSW traversal statistics as active result fields
- **AND** routing mode SHALL remain attributable to exact or two-level coarse routing

## REMOVED Requirements

### Requirement: HNSW coarse routing SHALL 作为可选 centroid routing backend 提供
**Reason**: HNSW centroid routing is no longer part of the formal search path and would add an unused approximate routing branch beside exact and two-level routing.
**Migration**: Use exact centroid scoring or supported two-level coarse routing.

### Requirement: HNSW graph SHALL 基于已加载的一层 centroids 构建并缓存
**Reason**: Query-time HNSW graph construction and caching are removed with the HNSW routing backend.
**Migration**: No graph warmup is required for formal runs.

### Requirement: HNSW graph SHALL 使用 metric-aware centroid input
**Reason**: Metric-aware HNSW graph input is obsolete once HNSW coarse routing is removed.
**Migration**: Metric-aware handling remains in exact/two-level centroid scoring only.

### Requirement: benchmark 输出 SHALL 支持 HNSW routing 复现和对比
**Reason**: HNSW routing is no longer an experimental or formal benchmark dimension.
**Migration**: Benchmark outputs SHALL keep exact/two-level routing metadata and omit HNSW-specific fields.
