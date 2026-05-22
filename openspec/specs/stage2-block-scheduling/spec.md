## ADDED Requirements

### Requirement: Stage2 scheduling SHALL operate on `block_id + lane_mask`
When serving a `v11` compact index, Stage2 scheduling SHALL map uncertain candidates to `batch_block_id` and `lane_id`, rather than representing Stage2 work only as a list of independent candidate pointers.

#### Scenario: Uncertain candidates are grouped by Stage2 block
- **WHEN** Stage1 emits uncertain candidates for a `v11` compact cluster
- **THEN** the query path SHALL group them by `batch_block_id`
- **AND** it SHALL derive lane membership within each block from `lane_id`

### Requirement: Stage2 kernel dispatch SHALL consume batch-block views
The `v11` Stage2 serving path SHALL dispatch the Stage2 kernel using a batch-block view plus a lane-selection signal, rather than passing eight unrelated `code_abs/sign` pointers as the primary hot-path representation.

#### Scenario: Stage2 kernel is called with a block view
- **WHEN** Stage2 boosting runs on a `v11` compact block
- **THEN** the query path SHALL call the Stage2 kernel with a batch-block-aligned Stage2 view
- **AND** it SHALL limit work to the lanes selected for that block

### Requirement: Block-aware Stage2 scheduling SHALL preserve funnel semantics
Moving from candidate-list Stage2 scheduling to block-aware scheduling SHALL NOT change Stage1 SafeOut / SafeIn / Uncertain semantics, final top-k semantics, or resident serving semantics.

#### Scenario: Block-aware scheduling preserves query results
- **WHEN** the same query is run against semantically equivalent `v10` and `v11` indexes under the same serving settings
- **THEN** Stage2 block-aware scheduling SHALL preserve the same recall and ranking semantics
- **AND** resident serving behavior SHALL remain compatible

### Requirement: Block-aware Stage2 scheduling SHALL ignore padded lanes
If a `v11` batch block contains padded lanes in the final block, Stage2 scheduling SHALL ensure those lanes are never treated as real candidates.

#### Scenario: Tail-block padding does not create fake candidates
- **WHEN** Stage2 evaluates the final compact batch block and that block contains padded lanes
- **THEN** the scheduler SHALL exclude padded lanes from Stage2 boosting and classification
- **AND** those lanes SHALL not appear in candidate, rerank, or benchmark statistics

## ADDED Requirements (from query-hotpath-submit-batch-stage2-refinement)

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
