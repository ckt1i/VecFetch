## ADDED Requirements

### Requirement: Resident submit batching SHALL support three-layer flush control
在 `full_preload + use_resident_clusters=1` 的 resident 查询路径下，系统 MUST 将 `submit-batch` 视为正式的三层 flush 机制，而不是单一静态阈值。该机制 MUST 至少区分 `hard flush`、`soft flush` 与 `tail flush` 三类触发，并以待提交工作量和资源状态决定何时提交。

#### Scenario: Hard flush is triggered by resource pressure
- **WHEN** 待提交请求数、待提交字节数、ring 容量或固定 buffer 状态达到实现定义的安全上限
- **THEN** 系统 MUST 立即执行 flush
- **AND** 不得继续积压这些请求等待后续 cluster

#### Scenario: Soft flush is triggered by profitable batch size
- **WHEN** resident 查询路径的 pending work 达到目标批量或提交间隔上限
- **THEN** 系统 MUST 允许执行一次 soft flush
- **AND** 该 flush MUST 保持现有请求语义与结果语义不变

#### Scenario: Tail flush prevents end-of-query underfilled batching
- **WHEN** 剩余 cluster 已不足以把 pending work 攒到目标批量
- **THEN** 系统 MUST 允许在 query 尾部提前 flush
- **AND** 不得为了继续攒批量而把尾部请求无界延迟

### Requirement: Submit batching SHALL distinguish normal probe flow from stop-sensitive flow
`submit-batch` 机制 MUST 同时兼容固定 `nprobe` 的完整探测路径与 CRC early-stop 路径。系统 MUST 允许在常规 probe 推进阶段使用普通三层 flush，并在进入 stop-sensitive 上下文时切换到更严格的 stop 前提交流程。

#### Scenario: Fixed-nprobe flow can finish with final flush
- **WHEN** 查询运行在固定 `nprobe` 的完整探测路径上
- **THEN** 系统 MAY 主要依赖普通三层 flush 推进请求
- **AND** 在 probe 循环结束后 MUST 执行 final flush 以收敛残余请求

#### Scenario: Early-stop flow enters stop-sensitive submit mode
- **WHEN** 查询运行在 CRC early-stop 路径上且一个 cluster 结束后即将执行正式 stop 判定
- **THEN** 系统 MUST 允许进入 stop-sensitive submit 上下文
- **AND** 不得仅依赖 query 末尾的兜底 flush 来保证 stop 判定有效

### Requirement: CRC early-stop path SHALL support stop-safe flush and break-time drain
在 CRC early-stop 路径上，系统 MUST 在 stop 判定前支持 `stop-safe flush`，并在决定提前停止后于退出 probe loop 前执行 `flush + drain`。该流程 MUST 保持 CRC cluster 边界语义、候选语义与最终结果语义不变。

#### Scenario: Stop-safe flush refreshes pending state before stop check
- **WHEN** CRC early-stop 路径在 cluster-end 准备执行 stop 判定且仍存在 prepped 或 pending 请求
- **THEN** 系统 MUST 允许先执行 `stop-safe flush`
- **AND** 之后再进行正式 stop 判定

#### Scenario: Break performs flush and drain before leaving probe loop
- **WHEN** CRC early-stop 判定决定提前结束 probe
- **THEN** 系统 MUST 在退出 probe loop 前执行 `flush + drain`
- **AND** 不得留下与当前 query 结果相关的未提交请求悬挂到 probe 生命周期之外

### Requirement: Fixed-parameter flush policy SHALL remain the primary first-stage mode
三层 flush 机制的第一阶段实现 MUST 支持固定参数策略，并将其作为默认验证路径。固定参数版 MUST 至少定义目标 batch、提交间隔上限和 tail 最小提交阈值，使基线实验能够在不依赖在线自适应的情况下复现。

#### Scenario: Fixed submit policy is reproducible across runs
- **WHEN** 使用相同 operating point 重复运行 resident benchmark
- **THEN** 系统 MUST 使用相同的固定 flush 参数
- **AND** benchmark 结果 MUST 足以复现实验结论

#### Scenario: Fixed policy remains valid without online predictors
- **WHEN** 在线观测调度未启用
- **THEN** resident 查询路径 MUST 仍能仅依靠固定参数执行完整 flush 决策
- **AND** 不得因为缺少在线统计而回退到逐 cluster 立即提交

### Requirement: Online-observation submit tuning SHALL remain lightweight and gated
系统 MAY 在固定参数版验证有效后启用轻量在线观测调度，但该调度 MUST 仅依赖 query-local 的轻量统计量，例如每 cluster 产出的 pending request 强度。在线观测版 MUST 保持与固定参数版相同的 flush 语义边界，并且不得引入新的结果语义。

#### Scenario: Online tuning adjusts target batch without changing flush classes
- **WHEN** 在线观测版根据 query-local 统计调整目标 batch 或 interval
- **THEN** 它 MUST 仍然只在 `hard flush`、`soft flush` 与 `tail flush` 这三类决策内工作
- **AND** 不得引入新的提交语义类别

#### Scenario: Online tuning can be disabled without changing correctness
- **WHEN** 在线观测调度被关闭
- **THEN** 系统 MUST 回退到固定参数版三层 flush
- **AND** 查询正确性、recall 和最终排序语义 MUST 保持不变
