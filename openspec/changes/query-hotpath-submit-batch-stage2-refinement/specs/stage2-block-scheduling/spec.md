## ADDED Requirements

### Requirement: Stage2 collect SHALL support block-first assembly
在当前 block-aware Stage2 调度路径上，系统 MUST 支持将 uncertain candidate 的 `collect` 从逐 survivor 线性拼装收敛到 block-first 组织方式。该方式 MUST 以 `block_id + lane membership` 为主表示，并允许先形成 per-block 选择信息，再填充 Stage2 block scratch。

#### Scenario: Collect groups survivor lanes by block before Stage2 dispatch
- **WHEN** 一个 Stage1 block 产生多个 uncertain candidate
- **THEN** Stage2 collect MUST 先按 `block_id` 组织这些 lane
- **AND** 不得要求每个 survivor 都通过线性扫描已有 block 列表才能找到归属

#### Scenario: Block-first collect preserves valid-lane semantics
- **WHEN** Stage2 collect 使用 block-first 组织
- **THEN** 它 MUST 继续正确排除 padding lane 或无效 lane
- **AND** 不得把这些 lane 暴露给 Stage2 kernel、candidate funnel 或 benchmark 统计

### Requirement: Stage2 scatter SHALL allow batch numeric classify before compact writeback
在 block-aware Stage2 路径中，系统 MUST 允许 scatter 先对一个 block 的批量 `ip_raw` 结果执行 batch numeric classify，再执行 surviving lane 的压缩写回。该能力 MUST 保持现有 funnel 语义与最终候选语义不变。

#### Scenario: Scatter can classify a whole block before writeback
- **WHEN** Stage2 kernel 为一个 batch block 产出批量 `ip_raw`
- **THEN** scatter MUST 允许先批量计算 `ip_est`、距离估计或等价分类边界
- **AND** 之后再按 surviving lane 压缩写回候选

#### Scenario: Batch classify does not change surviving candidate semantics
- **WHEN** scatter 改成 batch numeric classify
- **THEN** `SafeIn`、`SafeOut` 与 `Uncertain` 的分类结果 MUST 与参考路径保持语义一致
- **AND** 写回后的候选顺序与后续 funnel 语义 MUST 保持兼容
