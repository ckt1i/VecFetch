## ADDED Requirements

### Requirement: 查询后段必须支持批量 rerank
系统 MUST 支持先收集候选、后执行分组原始向量读取与批量 rerank 的执行模式，不再要求候选一到达就立即执行单条 `L2Sqr`。该能力 SHALL 适用于 end-to-end 查询路径中的 resident 模式优化阶段。

#### Scenario: 候选先收集后统一 rerank
- **WHEN** probe 阶段产出一批待精排候选
- **THEN** 系统必须能够先保留候选集合，再按批次执行原始向量读取和距离计算

### Requirement: 批量 rerank 必须保持 recall 与排序语义
批量 rerank 模式 MUST 保持与原有到达即精排模式一致的 recall 语义和最终排序语义，不得因批处理顺序变化而改变结果定义。

#### Scenario: 批处理不改变结果语义
- **WHEN** 使用同一索引、同一 query 集和同一参数比较批量 rerank 与原有 rerank 模式
- **THEN** 两者的 recall 口径和最终 top-k 语义必须一致

### Requirement: Payload 获取必须与批量 rerank 解耦
系统 MUST 将 payload 获取保持在最终结果阶段或与最终结果等价的后置阶段，不得因为引入批量 rerank 而对所有短命候选提前执行 payload 读取或缓存。

#### Scenario: 非最终候选不提前获取 payload
- **WHEN** 候选仅进入批量 rerank 阶段但未进入最终结果集合
- **THEN** 系统不得仅因为该候选参与过 rerank 就为其执行最终 payload 获取

### Requirement: 查询后段必须沿用现有 SafeIn / Uncertain 提交与解码路径
系统 MUST 在 probe 阶段继续沿用现有 SafeIn 和 Uncertain 各自的提交与解码方式：每 probe 完一个 cluster 后立即提交该 cluster 候选对应的 raw vector prefetch，并对被判定为 SafeIn 的少量候选沿用现有机制提前提交对应 payload prefetch。系统 MUST 在全部目标 cluster probe 完成后，再统一整理候选并从内存池读取预取好的原始向量执行一轮批量 rerank。

#### Scenario: probe 阶段沿用现有预取路径
- **WHEN** 单个 cluster probe 完成并产出候选
- **THEN** 系统必须沿用现有 SafeIn / Uncertain 提交与解码路径提交 raw vector 预取，并对 SafeIn 的少量候选沿用现有路径提交 payload 预取

#### Scenario: 区分磁盘预读与内存池读取
- **WHEN** 系统完成单个 cluster 的候选预取提交并在全部目标 cluster probe 完成后进入 batch rerank
- **THEN** 系统必须将"按 cluster 从磁盘向原始向量内存池预读"的阶段与"从原始向量内存池批量读取并组织 batch 输入"的阶段视为两个不同阶段

#### Scenario: 全 probe 完成后统一 batch rerank
- **WHEN** 全部目标 cluster 的 probe 完成
- **THEN** 系统必须在此之后统一整理候选，并从内存池读取已预取原始向量执行一轮批量 rerank

### Requirement: 最终 payload 获取必须区分 SafeIn 预取与剩余补取
系统 MUST 区分两类 payload 路径：对 SafeIn 的少量候选可沿用现有路径提前预取 payload；在最终结果阶段，系统 MUST 仅对仍未命中 payload 内存池的最终结果执行 remaining payload fetch。

#### Scenario: 最终阶段只补取剩余 payload
- **WHEN** batch rerank 得到最终结果集合
- **THEN** 系统必须优先复用 SafeIn 阶段已预取的 payload，并仅对剩余缺失项执行 remaining payload fetch

### Requirement: 批量 rerank 路径必须输出分段统计字段
系统 MUST 为该路径输出至少以下统计字段：`prefetch_submit_ms`、`prefetch_wait_ms`、`safein_payload_prefetch_ms`、`candidate_collect_ms`、`pool_vector_read_ms`、`rerank_compute_ms`、`remaining_payload_fetch_ms`、`num_candidates_buffered`、`num_candidates_reranked`、`num_safein_payload_prefetched`、`num_remaining_payload_fetches`。

#### Scenario: 批量 rerank 路径输出统计
- **WHEN** 查询以该批量 rerank 模式完成
- **THEN** 结果统计中必须包含预取、候选整理、内存池读取、批量计算和 remaining payload 补取相关字段

#### Scenario: 分离磁盘预读与内存池读取统计
- **WHEN** 查询输出分段统计
- **THEN** 统计结果必须能够区分 probe 阶段的磁盘 prefetch 预读相关开销与 all-probe-done 后的内存池批量读取相关开销

## ADDED Requirements (from probe-submit-fastpath-optimization)

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