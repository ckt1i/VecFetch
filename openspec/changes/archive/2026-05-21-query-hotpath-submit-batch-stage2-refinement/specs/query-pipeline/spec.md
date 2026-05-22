## ADDED Requirements

### Requirement: Query pipeline SHALL treat adaptive submit batching as the primary probe-submit follow-up path
查询主路径在 resident/full-preload 主验收模式下 MUST 将 `probe_submit` follow-up 优化收敛到自适应 submit batching，而不是继续把逐 cluster 立即提交或单一静态阈值视为唯一正式路径。该路径 MUST 保持 `SafeIn` / `Uncertain` 分类、rerank、payload 和最终排序语义不变。

#### Scenario: Adaptive submit changes scheduling but not candidate semantics
- **WHEN** resident 查询路径启用三层 flush 的 `submit-batch`
- **THEN** `SafeIn` 候选仍然 MUST 保持原有的 `ALL` 读取语义
- **AND** `Uncertain` 候选仍然 MUST 保持原有的 `VEC_ONLY` 读取语义
- **AND** rerank、payload 获取和最终 top-k 语义 MUST 与参考路径一致

### Requirement: Query pipeline SHALL preserve probe semantics under fixed-nprobe and CRC early-stop submit control
查询主路径在引入自适应 `submit-batch` 后 MUST 同时保持固定 `nprobe` 路径和 CRC early-stop 路径的 probe 语义。系统 MUST 允许在 fixed-nprobe 模式下通过 final flush 收敛残余请求，并在 early-stop 模式下通过 stop-sensitive flush 保持 stop 判定与请求生命周期一致。

#### Scenario: Fixed-nprobe submit control preserves full probe coverage
- **WHEN** 查询以固定 `nprobe` 模式运行
- **THEN** 自适应 submit 控制 MUST 不减少计划探测的 cluster 数
- **AND** probe 循环结束时 MUST 能通过 final flush 收敛残余请求

#### Scenario: Early-stop submit control preserves CRC stop boundary
- **WHEN** 查询以 CRC early-stop 模式运行
- **THEN** 自适应 submit 控制 MUST 不改变 CRC stop 的 cluster 边界语义
- **AND** stop 判定前后的 flush / drain 行为 MUST 不改变最终 recall 与排序语义

### Requirement: Query pipeline SHALL refine Stage2 through collect/scatter before layout escalation
查询主路径在推进 Stage2 follow-up 优化时 MUST 先把现有 Stage2 路径的 `collect` 与 `scatter` 视为正式优化边界，而不是直接要求新的 resident layout 或 compute-ready 视图。该顺序 MUST 保持现有 resident view、当前 kernel 入口和最终 funnel 语义兼容。

#### Scenario: Stage2 follow-up remains layout-compatible
- **WHEN** resident 查询路径继续优化 Stage2
- **THEN** 系统 MUST 允许仅通过 `collect` 组织和 `scatter` 数值分类的优化来推进
- **AND** 不得把新索引格式或 resident layout 重构作为该阶段的前置条件

#### Scenario: Stage2 refinement preserves funnel semantics
- **WHEN** Stage2 `collect` 或 `scatter` 被优化
- **THEN** Stage1 `SafeOut` / `SafeIn` / `Uncertain` funnel 语义 MUST 保持不变
- **AND** 最终 recall 与结果排序 MUST 与参考路径保持一致

### Requirement: Query pipeline follow-up work SHALL stay attributable to stable sub-phases
本轮 `submit-batch` 与 Stage2 follow-up 优化 MUST 继续挂接到稳定的 query 子阶段拆分上。系统 MUST 保持能够把收益归因到 `probe_submit`、Stage2 collect/scatter 或其他已定义的子阶段，而不是重新把这些成本混回粗粒度总时间。

#### Scenario: Follow-up optimization remains sub-phase attributable
- **WHEN** 后续实现改变 `submit-batch` 或 Stage2 `collect/scatter`
- **THEN** benchmark 和 profiling 输出 MUST 仍然能够定位变化影响的 query 子阶段
- **AND** 不得仅输出无法解释来源的单一总时间变化
