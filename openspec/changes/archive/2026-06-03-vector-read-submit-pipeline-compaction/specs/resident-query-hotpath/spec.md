## ADDED Requirements

### Requirement: Resident vector-only submit path SHALL support batch-oriented emit
在 `full_preload + use_resident_clusters=1` 的 resident 查询路径中，`VEC_ONLY` read submit path SHALL 支持 batch-oriented emit。该路径 MUST 在保留现有 pending queue、submit budget、partial flush、tail flush、final drain 和 in-flight accounting 的前提下，批量完成 vector-only address 读取、buffer 获取、pending slot 分配和 read prep。`VEC_ALL`、`PAYLOAD` 和 `CLUSTER_BLOCK` MUST 继续保持通用路径行为。

#### Scenario: Batch emit preserves submit budget
- **WHEN** `EmitPendingDataRequests` 被要求最多发射 `max_count` 个请求
- **THEN** batch-oriented vector-only emit MUST NOT 发射超过预算的请求
- **AND** 未发射的 vector-only requests MUST 保留到后续 flush

#### Scenario: Batch emit preserves tail flush semantics
- **WHEN** query 尾部 flush 仍存在 pending vector-only requests
- **THEN** batch-oriented vector-only emit MUST 精确发射剩余请求
- **AND** 不得漏发或重复发射任何 candidate read

#### Scenario: Non-vector request paths remain generic
- **WHEN** scheduler 发射 `VEC_ALL`、`PAYLOAD` 或 `CLUSTER_BLOCK` 请求
- **THEN** 这些请求 MUST 继续使用通用 pending state 和 cleanup 语义
- **AND** vector-only batch emit MUST NOT 改变它们的生命周期

### Requirement: Resident vector-only submit path SHALL use query-local emit scratch
resident vector-only emit SHALL use scheduler/query-local scratch storage to stage contiguous batches of `AddressEntry`、buffer pointer、fixed-buffer index、slot id 和 fixed/fallback 标记。该 scratch MUST 复用容量，不得在每个 read request 上产生动态分配；当待发射数量超过 scratch capacity 时，系统 MUST 分片处理而不是静默截断。

#### Scenario: Emit scratch is reused across queries
- **WHEN** 同一 scheduler 连续执行 resident queries
- **THEN** vector-only emit scratch MUST 复用已有容量
- **AND** 每次 query 或 emit batch MUST 重置本次使用的计数状态

#### Scenario: Emit scratch capacity does not truncate requests
- **WHEN** pending vector-only request count 超过 emit scratch capacity
- **THEN** 系统 MUST 分多批发射请求
- **AND** 最终发射请求数 MUST 与 pending request 数一致

### Requirement: Resident submit preparation SHALL allow direct AddressEntry staging
在 resident single-assignment 主路径中，candidate submit preparation SHALL allow direct staging of `AddressEntry` for safein-all and vector-only classes after dedup/classification. 实现 MAY 保留旧的 index-based scratch 作为 fallback，但主路径 SHOULD avoid repeated index indirection when compact address staging is available. 该优化 MUST 保持 duplicate filtering、SafeIn-all threshold、vector-only classification 和 final result 语义不变。

#### Scenario: Direct staging preserves vector-only candidates
- **WHEN** candidate batch 中存在 non-SafeOut vector-only candidates
- **THEN** direct address staging MUST 将这些 candidates 的 `AddressEntry` 写入 vector-only submit staging
- **AND** 后续 read plan 或 emit path MUST 发射等价的 vector reads

#### Scenario: Direct staging preserves safein-all candidates
- **WHEN** candidate batch 中存在满足 all-read threshold 的 SafeIn candidates
- **THEN** direct address staging MUST 将这些 candidates 的 `AddressEntry` 写入 all-read staging
- **AND** 后续 all-read request 的 length 和 offset MUST 与原路径一致

#### Scenario: Dedup semantics remain unchanged
- **WHEN** candidate batch 内或 query 内出现重复 address
- **THEN** direct address staging MUST 保持与原路径一致的 duplicate filtering 语义
- **AND** duplicate counters MUST 继续正确更新

### Requirement: Resident vector-only pending slot release SHALL avoid unnecessary generic state
resident vector-only completion path SHALL support a fast pending-slot allocation/release path that only records and clears vector-only completion state. The path MUST accurately preserve completion user-data to slot mapping, vector address, buffer ownership, fixed-buffer index, and cleanup behavior. Generic `PendingIO` state MUST remain available for non-vector-only requests.

#### Scenario: Vector-only fast slot contains required completion data
- **WHEN** vector-only batch emit allocates a pending slot
- **THEN** the slot MUST contain the vector address, buffer pointer, cleanup mode and fixed-buffer index if any
- **AND** completion dispatch MUST be able to call the same rerank vector-consumption semantics as before

#### Scenario: Vector-only fast release returns fixed buffer
- **WHEN** a fixed-buffer vector-only read completes
- **THEN** vector-only fast release MUST return the fixed buffer index to the fixed vector buffer free-list
- **AND** the pending slot MUST become reusable

#### Scenario: Vector-only fast release returns vec-only pool buffer
- **WHEN** a vec-only pool fallback read completes
- **THEN** vector-only fast release MUST return the buffer to the vec-only pool
- **AND** the pending slot MUST become reusable without entering generic buffer-pool cleanup

### Requirement: Fixed vector buffer count SHALL be validated for burst-sized vector-only reads
resident vector-only benchmark validation SHALL include fixed vector buffer counts large enough to cover the observed per-query vector-only read burst. Default configuration MUST remain compatible, but the official validation for this change SHALL include `fixed_vec_buffer_count=1024` in addition to the existing `512` point.

#### Scenario: Fixed buffer count 1024 is benchmarked
- **WHEN** validating this change on MSMARCO `fht_kac_rotator`
- **THEN** benchmark MUST include a run with `--fixed-vec-buffer-count 1024`
- **AND** output MUST report real recall and fixed-buffer hit/miss counters

#### Scenario: Default fixed buffer behavior remains compatible
- **WHEN** `fixed_vec_buffer_count` is 0
- **THEN** scheduler MUST preserve the existing default behavior based on `io_queue_depth`
- **AND** correctness MUST NOT depend on explicitly setting 1024
