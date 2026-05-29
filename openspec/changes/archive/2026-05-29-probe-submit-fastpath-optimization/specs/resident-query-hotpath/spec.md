## ADDED Requirements

### Requirement: Resident vector-only submit path SHALL support a compact fast path
在 `full_preload + use_resident_clusters=1` 且 single-assignment 的 resident 查询模式下，系统 SHALL 支持一个 compact 的 vector-only submit fast path，在存在等价语义时避开通用的 read-plan 和 pending-I/O 状态。该 fast path MUST 保留候选有效性、受控模式下的重复处理假设，以及最终结果语义。

#### Scenario: Resident single-assignment queries use the vector-only fast path
- **WHEN** 查询运行于 resident full-preload 且 single-assignment 模式
- **THEN** vector-only 候选读取 SHALL 具备 compact submit fast path 的使用资格
- **AND** 查询结果 MUST 与现有通用 submit 路径保持等价

#### Scenario: Generic submit path remains available outside controlled mode
- **WHEN** resident 模式、full-preload 模式或 single-assignment 条件不满足
- **THEN** 系统 SHALL 继续使用现有通用 submit 路径
- **AND** 不得要求 vector-only fast-path 假设来保证正确性

### Requirement: Resident vector-only read plans SHALL use reusable queue storage
resident 的 vector-only submit path SHALL 将 pending vector-only read plan 存入可复用的队列存储，例如 vector-backed ring 或 head-index queue，而不是在热路径中依赖 per-query 的 `std::deque` churn。

#### Scenario: Query reset reuses vector-only plan capacity
- **WHEN** scheduler 执行多个 resident query
- **THEN** vector-only read-plan queue SHALL 复用已有容量
- **AND** 每个 query MUST 在开始时重置队列状态，且不得把旧 read plan 泄漏到下一次 query

#### Scenario: Tail flush preserves pending vector-only requests
- **WHEN** query 在尾部 flush 时仍有未发出的 vector-only 请求
- **THEN** 可复用队列 SHALL 精确发射剩余请求
- **AND** 不得漏发或重复发出任何候选读取

### Requirement: Resident vector-only pending slots SHALL avoid unnecessary generic state
vector-only submit fast path SHALL 提供一种轻量 pending-slot 表示或 allocation helper，只保存 vector completion handling 所需状态。通用 `PendingIO` 状态 MUST 继续保留给 cluster blocks、all-read requests、payload requests 和 fallback 行为。

#### Scenario: Vector-only slot records required completion state
- **WHEN** 通过 fast path 发射 vector-only read
- **THEN** pending slot SHALL 记录地址、buffer 指针、cleanup 模式，以及适用时的 fixed-buffer index
- **AND** completion dispatch MUST 具备调用与通用路径相同的 vector-consumption 语义所需的信息

#### Scenario: Non-vector requests keep generic pending state
- **WHEN** scheduler 发射 cluster-block、all-read 或 payload 请求
- **THEN** 这些请求 SHALL 保持通用 pending state 和所有权行为
- **AND** vector-only fast path MUST NOT 改变它们的生命周期
