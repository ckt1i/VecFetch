## ADDED Requirements

### Requirement: HNSW coarse routing SHALL 作为可选 centroid routing backend 提供

系统 SHALL 为 IVF 查询时的 coarse select 提供一个可选的 HNSW centroid routing backend，同时保留 exact 和 two-level coarse routing 路径。

#### Scenario: HNSW routing 默认关闭
- **WHEN** 用户未显式开启 HNSW coarse routing
- **THEN** 系统 SHALL 保持现有 exact/two-level routing 行为不变

#### Scenario: HNSW routing 被显式开启
- **WHEN** 用户开启 HNSW coarse routing 且当前 index 满足支持条件
- **THEN** `FindNearestClusters()` SHALL 使用 HNSW graph over IVF centroids 返回 top-`nprobe` cluster IDs
- **AND** 返回结果 SHALL 继续映射到现有 `cluster_ids_`

#### Scenario: HNSW 不可用时回退 exact
- **WHEN** HNSW graph 构建失败、未 ready、返回结果不足或 label 越界
- **THEN** 系统 SHALL 回退到 exact coarse select
- **AND** 查询 SHALL 继续返回有效 cluster IDs

### Requirement: HNSW graph SHALL 基于已加载的一层 centroids 构建并缓存

系统 SHALL 在内存中基于已加载的一层 IVF centroids 构建 HNSW graph，并在多个查询之间复用。

#### Scenario: graph 不按 query 重建
- **WHEN** 多个查询使用相同 index 和相同 HNSW build 配置
- **THEN** 系统 SHALL 复用已构建的 HNSW graph
- **AND** 它 SHALL NOT 每个查询重新构建 graph

#### Scenario: benchmark warmup 构建 graph
- **WHEN** `bench_e2e` 开启 HNSW coarse routing
- **THEN** benchmark SHALL 在正式计时 query round 前构建 HNSW graph
- **AND** graph build time SHALL 单独统计，不混入 steady-state query latency

### Requirement: HNSW graph SHALL 使用 metric-aware centroid input

系统 SHALL 根据 index metric 选择 HNSW graph 的输入 centroid 表示和 Faiss metric。

#### Scenario: cosine/IP 索引使用 normalized centroids
- **WHEN** `requested_metric == "cosine"` 且 `effective_metric == "ip"`
- **THEN** HNSW graph SHALL 使用 `normalized_centroids_`
- **AND** query SHALL 先 normalize 后再执行 HNSW search

#### Scenario: L2/IP 非 cosine 索引使用对应 metric
- **WHEN** 当前 index 使用 L2 或非 cosine IP
- **THEN** HNSW graph SHALL 使用对应的 centroid 表示和 Faiss metric
- **AND** 如果该路径未被首版支持，则 SHALL 回退 exact 并记录诊断

### Requirement: benchmark 输出 SHALL 支持 HNSW routing 复现和对比

benchmark 输出 SHALL 包含 HNSW coarse routing 的配置和诊断字段，用于与 exact 和 two-level routing 对比。

#### Scenario: 导出 HNSW 配置
- **WHEN** HNSW coarse routing 被开启
- **THEN** 输出 SHALL 包含 HNSW enable、M、efConstruction 和 efSearch

#### Scenario: 导出 HNSW 诊断
- **WHEN** HNSW coarse routing 被运行
- **THEN** 输出 SHALL 包含 routing mode、graph build time、fallback count 和 coarse timing 字段

#### Scenario: 正式性能结论使用真实 recall
- **WHEN** HNSW benchmark 结果用于性能结论
- **THEN** 运行 SHALL 使用真实 GT recall
- **AND** 运行 SHALL 显式关闭 early stop
