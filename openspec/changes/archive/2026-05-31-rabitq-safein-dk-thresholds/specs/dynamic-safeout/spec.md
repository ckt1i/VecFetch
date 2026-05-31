## MODIFIED Requirements

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
