## MODIFIED Requirements

### R1: ClassifyAdaptive 方法
ConANN 类提供 `ClassifyAdaptive` 或等价运行时分类接口，查询路径的 SafeIn/SafeOut MUST 使用候选区间边界语义：
- SafeOut: `approx_dist - safeout_margin > safeout_frontier_upper`
- SafeIn: `approx_dist + safein_margin < fullvector_safein_d_k`
- Uncertain: 其余情况

现有 `ClassifyAdaptive(approx_dist, margin, dynamic_d_k)` overload MAY 保持源码兼容，但查询路径 MUST NOT 继续把同一个 candidate margin 同时用于候选和 top-k frontier 的 `approx_dist > dynamic_d_k + 2 * margin` 语义。

#### Scenario: SafeOut 使用候选下界
- **WHEN** 候选的估计距离为 `approx_dist`，SafeOut 误差半径为 `safeout_margin`
- **THEN** 候选 MUST 仅在 `approx_dist - safeout_margin > safeout_frontier_upper` 时分类为 SafeOut

#### Scenario: SafeIn 使用候选上界
- **WHEN** 候选的估计距离为 `approx_dist`，SafeIn 误差半径为 `safein_margin`
- **THEN** 候选 MUST 仅在 `approx_dist + safein_margin < fullvector_safein_d_k` 时分类为 SafeIn

#### Scenario: 区间无法安全判定
- **WHEN** 候选区间既不满足 SafeOut 下界规则，也不满足 SafeIn 上界规则
- **THEN** 候选 MUST 分类为 Uncertain

### R2: dynamic_d_k 来源
SafeOut frontier SHALL 基于 query-time estimate heap 以及其中保留候选的误差界导出：
- `est_heap_` entries MUST retain both `d_hat` and the candidate error radius `e`.
- When `est_heap_.size() >= top_k`, runtime MUST compute `safeout_frontier_upper = est_heap_.front().d_hat + max_e_in_est_heap`.
- When `est_heap_.size() < top_k`, `safeout_frontier_upper` MUST be positive infinity or an equivalent sentinel.
- Runtime MUST NOT fall back to static build-time `d_k` for estimate-driven SafeOut when the estimate heap is not full.

#### Scenario: heap 充满时使用 relaxed top-k upper frontier
- **WHEN** 进入 cluster 时 `est_heap_.size() >= top_k`
- **THEN** SafeOut MUST use `est_heap_.front().d_hat + max_e_in_est_heap` as `safeout_frontier_upper`

#### Scenario: heap 未满时禁用 estimate-driven SafeOut
- **WHEN** 进入 cluster 时 `est_heap_.size() < top_k`
- **THEN** `safeout_frontier_upper` MUST be positive infinity or an equivalent sentinel
- **AND** no candidate MUST be pruned by estimate-driven SafeOut

#### Scenario: heap entry 更新后维护最大误差
- **WHEN** estimate heap insertion or replacement changes retained heap entries
- **THEN** runtime MUST update `max_e_in_est_heap` so it equals the maximum error radius among retained heap entries

### R3: 更新粒度
SafeOut frontier SHALL 保持 cluster 级快照语义：
- `safeout_frontier_upper` MUST be snapshotted once when entering `ProbeCluster`.
- Stage1 and Stage2 classification inside the same cluster MUST use the same snapshotted frontier.
- Estimate heap updates produced by a cluster MUST affect only subsequently probed clusters.

#### Scenario: cluster 级 frontier 快照
- **WHEN** query pipeline starts probing a cluster
- **THEN** it MUST snapshot the current `safeout_frontier_upper` once
- **AND** all batches inside that cluster MUST use the snapshotted value

#### Scenario: cluster 结束后更新后续 frontier
- **WHEN** candidates from a cluster are merged into the estimate heap
- **THEN** the updated heap and `max_e_in_est_heap` MUST be visible to subsequently probed clusters

### R4: 向后兼容
现有 `Classify(approx_dist)` 和 `Classify(approx_dist, margin)` API SHALL 保持源码兼容。对于不携带新 SafeIn acceptance threshold 的旧索引，运行时 MUST 继续可用，并使用已有的 legacy exact-distance d_k 作为 full-vector SafeIn acceptance fallback。

#### Scenario: legacy classification APIs 继续可用
- **WHEN** existing callers use `Classify(approx_dist)` or `Classify(approx_dist, margin)`
- **THEN** these APIs MUST remain source-compatible

#### Scenario: old index 使用 legacy exact d_k 作为 SafeIn fallback
- **WHEN** runtime opens an index without an explicit full-vector SafeIn acceptance threshold
- **THEN** query execution MUST remain valid
- **AND** SafeIn MUST use the available legacy exact-distance d_k as fallback acceptance threshold

## ADDED Requirements

### Requirement: FastScan classification SHALL preserve estimate-kernel optimization boundaries
FastScan Stage1 分类阈值的修改 SHALL NOT 合并或模糊 single-bit estimate kernel 的优化边界。运行时 MAY 调整 SafeIn/SafeOut mask 阈值，但 `EstimateDistanceFastScan` 及相关 packed-code / LUT estimate 工作 MUST 继续保持可独立观测和优化。

#### Scenario: FastScan estimate kernel remains separate
- **WHEN** SafeIn/SafeOut threshold formulas are updated
- **THEN** the Stage1 estimate kernel MUST remain a distinct implementation and profiling boundary
- **AND** classification mask logic MUST remain separable from distance estimate generation

### Requirement: COCO100k vector-only validation SHALL report two-stage classification distribution
该变更 SHALL 包含一个可重复的仅向量验证：在 COCO100k 上运行 `bench_vector_search.cpp`，配置 `nlist=2048`、`nprobe=64`。验证输出 MUST 报告新边界逻辑下 Stage1 和 Stage2 的 SafeIn / SafeOut / Uncertain 数量。

#### Scenario: COCO100k nlist2048 nprobe64 validation
- **WHEN** `bench_vector_search.cpp` is rerun on COCO100k in vector-only mode with `nlist=2048` and `nprobe=64`
- **THEN** the results MUST include Stage1 SafeIn, SafeOut, and Uncertain counts
- **AND** the results MUST include Stage2 SafeIn, SafeOut, and Uncertain counts when Stage2 is enabled
- **AND** the validation notes MUST record the new SafeIn/SafeOut boundary mode used for the run
