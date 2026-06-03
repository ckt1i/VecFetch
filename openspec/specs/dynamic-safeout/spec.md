# Dynamic SafeOut

## Requirements

### R1: ClassifyAdaptive 方法
ConANN 类提供 `ClassifyAdaptive(approx_dist, margin, dynamic_d_k)` 方法：
- SafeOut: 当 `dynamic_d_k` 为有限值时，`approx_dist > dynamic_d_k + 2 * margin`
- SafeIn: `approx_dist < safein_d_k - 2 * margin`，其中 `safein_d_k` 来自 SafeIn 专用阈值；若索引没有 SafeIn 专用阈值，则 fallback 到 legacy `d_k_`
- Uncertain: 其余情况

#### Scenario: SafeOut 使用动态 kth 阈值
- **WHEN** `ClassifyAdaptive` 收到有限的 `dynamic_d_k`
- **THEN** SafeOut MUST 由 `approx_dist > dynamic_d_k + 2 * margin` 决定
- **AND** SafeOut MUST NOT 再使用 legacy `d_k_` 或 SafeIn `d_k` 作为额外下界

#### Scenario: SafeIn 使用 SafeIn 专用 d_k
- **WHEN** SafeIn-specific `safein_d_k` 可用
- **THEN** SafeIn MUST 由 `approx_dist < safein_d_k - 2 * margin` 决定
- **AND** SafeIn MUST NOT 使用 query-time `dynamic_d_k`

#### Scenario: SafeIn 回退到 legacy index
- **WHEN** 没有 SafeIn-specific `safein_d_k`
- **THEN** SafeIn MUST 使用 legacy `d_k_` 作为阈值

### R2: dynamic_d_k 来源
- 来自 OverlapScheduler 的 est_heap_（RaBitQ estimate max-heap）
- 当 `est_heap_.size() >= top_k` 时：`dynamic_d_k = est_heap_.front().first`
- 当 `est_heap_` 未满时：`dynamic_d_k = infinity`，表示 SafeOut 不应由静态 `d_k` fallback 触发

#### Scenario: heap 充满时使用 query-time kth
- **WHEN** 进入 cluster 时 `est_heap_.size() >= top_k`
- **THEN** `dynamic_d_k` MUST 等于 `est_heap_.front().first`
- **AND** 它 MUST NOT 被 legacy `d_k_` 或 SafeIn `d_k` 往上夹高

#### Scenario: heap 未满时禁用 SafeOut 阈值判断
- **WHEN** 进入 cluster 时 `est_heap_.size() < top_k`
- **THEN** `dynamic_d_k` MUST 是 infinity 或等价 sentinel
- **AND** SafeOut MUST NOT 由一个静态构建期 `d_k` 触发

### R3: 更新粒度
- `dynamic_d_k` 仅在 cluster 之间更新（进入 ProbeCluster 时读取一次）
- cluster 内的多个 batch 间不重新计算 `dynamic_d_k`
- `est_heap_` 本身在每个 vector 处理后持续更新

#### Scenario: cluster 级动态阈值快照
- **WHEN** 开始 probe 一个 cluster
- **THEN** query pipeline MUST 只快照一次当前 estimated kth 阈值
- **AND** 该 cluster 内所有 batch MUST 使用同一个 `dynamic_d_k`

### R4: 向后兼容
- 现有 `Classify(approx_dist)` 和 `Classify(approx_dist, margin)` 接口保持不变
- 旧索引没有 SafeIn 专用 `d_k` 时，SafeIn 使用 legacy `d_k_`
- 新的动态 SafeOut 行为 MUST 在实现 / 配置层面可控，以便 benchmark 比较旧模式和新模式

#### Scenario: legacy classification APIs 继续可用
- **WHEN** 现有调用方使用 `Classify(approx_dist)` 或 `Classify(approx_dist, margin)`
- **THEN** 这些 API MUST 保持源码兼容

#### Scenario: old index 可以在新 runtime 下运行
- **WHEN** runtime 打开一个在 SafeIn-specific `d_k` metadata 之前构建的索引
- **THEN** 查询执行 MUST 仍然有效
- **AND** SafeIn MUST 回退到 legacy `d_k_`

## MODIFIED Requirements (from safe-boundary-error-frontier)

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

## MODIFIED Requirements (from decouple-crc-safeout-frontier)

### R1: ClassifyAdaptive uses interval-bound SafeIn and SafeOut classification
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

### R2: Dynamic SafeOut frontier uses top-k candidate upper bounds
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

### R3: SafeOut frontier uses cluster-level snapshots
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

### R4: Dynamic SafeOut remains configurable and backward compatible
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
