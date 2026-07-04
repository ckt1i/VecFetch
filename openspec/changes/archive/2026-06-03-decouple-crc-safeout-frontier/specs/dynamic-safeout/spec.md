## MODIFIED Requirements

### Requirement: ClassifyAdaptive uses interval-bound SafeIn and SafeOut classification
ConANN 类提供 `ClassifyAdaptive` 或等价运行时分类接口，查询路径的 SafeOut MUST 使用候选下界和 dynamic upper frontier 的区间语义：
- SafeOut: 当 `safeout_frontier_upper` 为有限值时，`approx_dist - safeout_margin > safeout_frontier_upper`
- SafeIn: `approx_dist + safein_margin < safein_d_k`，其中 `safein_d_k` 来自 SafeIn 专用阈值；若索引没有 SafeIn 专用阈值，则 fallback 到 legacy `d_k_`
- Uncertain: 其余情况

现有 `Classify(approx_dist)`、`Classify(approx_dist, margin)` 或旧 overload SHALL 保持源码兼容，但查询路径 MUST NOT 继续把 SafeOut 写成依赖静态 `d_k_` 的分类，也 MUST NOT 把 CRC early-stop 状态作为 SafeOut 的必要条件。

#### Scenario: SafeOut 使用候选下界
- **WHEN** 候选估计距离为 `approx_dist`，SafeOut 误差半径为 `safeout_margin`，且 `safeout_frontier_upper` 为有限值
- **THEN** 候选 MUST 仅在 `approx_dist - safeout_margin > safeout_frontier_upper` 时分类为 SafeOut
- **AND** 等价阈值形式 MUST 为 `approx_dist > safeout_frontier_upper + safeout_margin`

#### Scenario: SafeIn 使用 SafeIn 专用 d_k
- **WHEN** SafeIn-specific `safein_d_k` 可用
- **THEN** SafeIn MUST 由 `approx_dist + safein_margin < safein_d_k` 决定
- **AND** SafeIn MUST NOT 使用 query-time `safeout_frontier_upper`

#### Scenario: SafeIn 回退到 legacy index
- **WHEN** 没有 SafeIn-specific `safein_d_k`
- **THEN** SafeIn MUST 使用 legacy `d_k_` 作为阈值

#### Scenario: 区间无法安全判定
- **WHEN** 候选既不满足 SafeOut 下界规则，也不满足 SafeIn 上界规则
- **THEN** 候选 MUST 分类为 Uncertain

### Requirement: Dynamic SafeOut frontier uses top-k candidate upper bounds
SafeOut frontier SHALL 来源于独立的 dynamic SafeOut frontier state，而不是 CRC early-stop state。frontier state MUST 基于候选 upper bound 维护当前 query 已见候选中的 top-k：
- 每个 frontier entry MUST 包含 `d_hat`、`e` 和 `U = d_hat + e`
- frontier state MUST 保留 `U` 最小的 top-k 个 entry
- 当 frontier state 满足 `top_k` 个 entry 时：`safeout_frontier_upper = max(U_j)`，即 `kth_smallest(U)`
- 当 frontier state 未满时：`safeout_frontier_upper = +inf` 或等价 sentinel，表示禁用 estimate-driven SafeOut

#### Scenario: heap 充满时使用 upper-bound kth frontier
- **WHEN** dynamic SafeOut frontier state 已保留至少 `top_k` 个 entry
- **THEN** `safeout_frontier_upper` MUST 等于这些 retained top-k upper-bound entries 中最大的 `U`
- **AND** 该值 MUST 等价于当前已见候选中的 `kth_smallest(d_hat + e)`

#### Scenario: heap 按 upper bound 选择 top-k
- **WHEN** 新候选的 `U = d_hat + e` 小于当前 retained top-k 中最大的 `U`
- **THEN** frontier state MUST 用该候选替换当前最大的 retained upper-bound entry
- **AND** frontier state MUST NOT 使用 `d_hat` 单独作为 SafeOut frontier heap 的排序 key

#### Scenario: heap 未满时禁用 estimate-driven SafeOut
- **WHEN** dynamic SafeOut frontier state 中 entry 数量小于 `top_k`
- **THEN** `safeout_frontier_upper` MUST 是 positive infinity 或等价 sentinel
- **AND** no candidate MUST be pruned by estimate-driven SafeOut

#### Scenario: CRC disabled 仍可维护 SafeOut frontier
- **WHEN** dynamic SafeOut 已启用，但 CRC early-stop 未启用或 `crc_params` 不存在
- **THEN** runtime MUST still maintain the dynamic SafeOut frontier state
- **AND** SafeOut classification MUST use a finite `safeout_frontier_upper` once the frontier state is full

### Requirement: SafeOut frontier uses cluster-level snapshots
SafeOut frontier SHALL 保持 cluster 级快照语义：
- `safeout_frontier_upper` MUST be snapshotted once when entering `ProbeCluster`
- Stage1 and Stage2 classification inside the same cluster MUST use the same snapshotted frontier
- Candidate estimates emitted by the current cluster MUST update the frontier only after cluster-local classification is complete
- Updated frontier MUST affect only subsequently probed clusters

#### Scenario: cluster 级动态阈值快照
- **WHEN** query pipeline starts probing a cluster
- **THEN** it MUST snapshot the current `safeout_frontier_upper` once
- **AND** all Stage1 and Stage2 classification inside that cluster MUST use the snapshotted value

#### Scenario: cluster 结束后更新后续 frontier
- **WHEN** candidates from a cluster survive SafeOut classification and are emitted to the query pipeline
- **THEN** their final-stage `d_hat` and `e` MUST be merged into the dynamic SafeOut frontier state before the next cluster snapshot

### Requirement: Dynamic SafeOut remains configurable and backward compatible
现有 `Classify(approx_dist)` 和 `Classify(approx_dist, margin)` API SHALL 保持源码兼容。新的 dynamic SafeOut 行为 MUST 在配置层面可控，以便 benchmark 比较 no-SafeOut、dynamic-SafeOut-only、CRC-only 和 CRC+dynamic-SafeOut 工作点。

#### Scenario: legacy classification APIs 继续可用
- **WHEN** existing callers use `Classify(approx_dist)` or `Classify(approx_dist, margin)`
- **THEN** these APIs MUST remain source-compatible

#### Scenario: old index 可以在新 runtime 下运行
- **WHEN** runtime opens an index without explicit SafeIn-specific metadata
- **THEN** query execution MUST remain valid
- **AND** SafeIn MUST fall back to the available legacy `d_k_`

#### Scenario: dynamic SafeOut 可独立关闭
- **WHEN** dynamic SafeOut is explicitly disabled in query configuration
- **THEN** runtime MUST use `+inf` or an equivalent sentinel as `safeout_frontier_upper`
- **AND** estimate-driven SafeOut MUST NOT prune candidates
