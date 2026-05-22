## ADDED Requirements

### Requirement: Batched rerank SHALL support reusable per-query vector storage
批量 rerank pipeline SHALL 支持可复用的 per-query 向量存储，使 buffered candidate 的 completion handling 不再需要对每个 vector-only 候选都执行一次 aligned allocation。该存储 MUST 保留现有的批量 rerank 行为和最终 top-k 语义。

#### Scenario: Vector completions use reusable rerank storage
- **WHEN** vector-only read completion 被消费并进入后续批量 rerank
- **THEN** 系统 SHALL 能够把向量复制到可复用的 per-query 存储中
- **AND** steady state 下不得要求每个候选都重新执行一次 aligned allocation

#### Scenario: Batched rerank semantics remain unchanged
- **WHEN** 使用可复用向量存储的候选进入批量 rerank
- **THEN** 最终 collector 的排序和 recall 语义 MUST 与现有 buffered-candidate rerank 路径一致
- **AND** candidate address 必须与正确复制的向量保持关联

### Requirement: Rerank vector storage SHALL have explicit query lifetime
可复用的 rerank 向量存储 SHALL 在 query 边界重置，并且 SHALL 保证向量数据在 `ExecuteBuffered` 完成消费所有 buffered candidate 之前一直有效。

#### Scenario: Query reset clears slab state
- **WHEN** 新 query 开始并复用同一 scheduler 或 rerank consumer 容量
- **THEN** 可复用 rerank 向量存储 SHALL 重置其分配游标或等价状态
- **AND** 上一个 query 的向量 MUST NOT 对新 query 可见

#### Scenario: Buffered vectors remain valid through rerank
- **WHEN** vector read buffer 在 completion dispatch 后被释放
- **THEN** 复制到可复用 rerank 存储中的向量数据 SHALL 在批量 rerank 完成前保持有效
- **AND** 释放 io_uring read buffer MUST NOT 使 pending rerank 输入失效
