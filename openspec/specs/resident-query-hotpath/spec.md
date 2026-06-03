## ADDED Requirements

### Requirement: Resident query path SHALL use a reusable thin query wrapper
在 `clu_read_mode=full_preload` 且 `use_resident_clusters=1` 的条件下，系统 MUST 进入 resident 专用 query hot path。该路径 MUST 使用可复用的 query wrapper / scratch 上下文来承载 query 期间的临时状态，并将 query 无关或容量相关的结构预先保留到 prepare/init 阶段，而不是在每个 query 内重复构造。

#### Scenario: Resident hot path is selected under resident serving
- **WHEN** benchmark 或在线查询使用 `full_preload + use_resident_clusters=1`
- **THEN** 查询实现 MUST 走 resident 专用 hot path
- **AND** 它 MUST 不再回退到仅为通用 cluster 读取路径设计的厚包装流程

#### Scenario: Query wrapper is reused across resident queries
- **WHEN** 同一线程或同一调度上下文连续执行多个 resident query
- **THEN** query wrapper / scratch 容器 MUST 复用既有容量和布局
- **AND** 每次 query 只覆盖本次所需内容，而不是重新构造整套中间对象

### Requirement: Resident single-assignment submit path SHALL support lightweight candidate organization
在 resident 且 single-assignment 的条件下，候选提交路径 MUST 支持轻量提交组织方式，以减少保守的全局去重、临时对象分配和高频小粒度提交带来的 CPU 开销。该轻量路径 MUST 保持候选语义与结果正确性不变，并在不满足条件时可回退到通用路径。

#### Scenario: Lightweight submit path is used in resident single-assignment mode
- **WHEN** 查询路径满足 resident 且 single-assignment 条件
- **THEN** 系统 MUST 允许使用轻量的候选提交组织方式
- **AND** 该方式 MUST 维持与现有路径一致的候选有效性语义

#### Scenario: Fallback remains available outside the controlled mode
- **WHEN** 查询路径不满足 resident 或 single-assignment 条件
- **THEN** 系统 MUST 可以继续使用原有通用提交路径
- **AND** resident 轻量路径 MUST 不强制外溢到其他 serving 模式

## ADDED Requirements (from fastscan-prepare-hotpath-optimization)

### Requirement: Resident query hot path SHALL reuse fixed-capacity prepare scratch in steady state
在 `full_preload + use_resident_clusters=1` 的 resident 查询 hot path 中，prepare 相关的 `PreparedQuery` 与 scratch 缓冲 MUST 支持 steady-state 固定容量复用。查询执行期间，系统 MUST 允许只覆盖本次 prepare 内容，而不是在每个 cluster 上重复执行可前移的 `resize`、对齐重算或同类容器状态维护。

#### Scenario: Resident prepare scratch reuses capacity across probed clusters
- **WHEN** 同一 resident query 连续 prepare 多个 cluster
- **THEN** `PreparedQuery` 与 scratch 缓冲 MUST 复用既有容量和布局
- **AND** 主路径 MUST 不要求在每个 cluster 上重新建立等价的容器状态

### Requirement: Resident query hot path SHALL avoid redundant prepare-buffer clearing when full overwrite is available
在 resident hot path 下，若 prepare 阶段的某个输出缓冲可以由后续计算完整覆盖写入，系统 SHALL 允许省去该缓冲在热路径中的冗余预清理。若存在尾部或部分覆盖边界，系统 MUST 仅对必要部分保留显式清理或 masking。

#### Scenario: Full-overwrite prepare buffer skips redundant clear
- **WHEN** resident prepare 路径生成一个能够被完整覆盖写的输出缓冲
- **THEN** 系统 MAY 跳过该缓冲在热路径中的整段预清理
- **AND** 最终输出语义 MUST 与参考路径保持一致

## ADDED Requirements (from query-hotpath-submit-batch-stage2-refinement)

### Requirement: Resident hot path SHALL support fixed-parameter three-layer submit control
在 `full_preload + use_resident_clusters=1` 的 resident hot path 中，轻量候选提交路径 MUST 支持固定参数版的三层 flush 机制，并将其视为 steady-state 的正式提交控制方式。该机制 MUST 在可复用 query wrapper / scratch 条件下工作，而不是通过重新构造通用提交流程来实现。

#### Scenario: Resident thin path uses three-layer submit control
- **WHEN** resident 查询路径在受控模式下运行
- **THEN** 它 MUST 使用 fixed-parameter `hard flush / soft flush / tail flush` 控制提交
- **AND** 该控制 MUST 运行在 resident thin path 内部

#### Scenario: Resident hot path can retain wrapper reuse under adaptive submit
- **WHEN** resident 查询路径切换到三层 flush 提交
- **THEN** query wrapper / scratch 的复用语义 MUST 保持有效
- **AND** 不得因为新的提交调度而退回到为通用路径准备的厚包装流程

### Requirement: Resident hot path SHALL support stop-sensitive flush for CRC early-stop
在 resident hot path 的 CRC early-stop 路径中，系统 MUST 支持 stop-sensitive flush 控制。该控制 MUST 在 cluster-end stop 判定前提供 `stop-safe flush`，并在决定提前停止后于退出 probe loop 前执行 `flush + drain`，同时保持 resident thin path 语义与 wrapper 复用语义不变。

#### Scenario: Resident early-stop path can flush before CRC check
- **WHEN** resident 查询路径在 CRC early-stop 模式下完成一个 cluster 且即将做 stop 判定
- **THEN** 它 MUST 允许执行 `stop-safe flush`
- **AND** 该动作 MUST 仍运行在 resident thin path 内部

#### Scenario: Resident early-stop path drains before break
- **WHEN** resident 查询路径在 CRC early-stop 模式下决定 break probe loop
- **THEN** 它 MUST 在 break 前执行 `flush + drain`
- **AND** 不得因为该动作而退回到非 resident 的通用查询包装流程

### Requirement: Resident hot path SHALL keep online-observation submit tuning reversible
resident hot path 若引入在线观测版 submit 调度，MUST 保持 query-local、轻量且可关闭。关闭在线观测调度后，系统 MUST 无缝回退到固定参数版三层 flush，而不改变 resident thin path 的其余语义。

#### Scenario: Online tuning is optional in resident mode
- **WHEN** resident 查询路径未启用在线观测调度
- **THEN** 系统 MUST 继续使用固定参数版三层 flush
- **AND** resident thin path 的候选有效性、dedup 和 rerank 语义 MUST 保持不变

#### Scenario: Online tuning does not widen resident path scope
- **WHEN** 在线观测调度被启用
- **THEN** 它 MUST 仅影响 resident 受控模式下的 submit 时机
- **AND** 不得把该机制强制外溢到非 resident 或非 thin-path 路径

## ADDED Requirements (from probe-submit-fastpath-optimization)

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

## ADDED Requirements (from vec-only-buffer-lifecycle-fastpath)

### Requirement: Resident vector-only fallback reads SHALL use fixed-size buffer lifecycle fast path
在 resident/full-preload 查询路径中，`VEC_ONLY` fallback reads SHALL 支持固定尺寸的 buffer 生命周期 fast path。该 fast path MUST 针对 `vec_bytes` 复用 4096-byte aligned buffer，并避免为每个 vector-only fallback read 进入通用多尺寸 buffer pool 的 capacity 扫描和 outstanding hash-map 维护。

#### Scenario: Vector-only fixed-buffer miss uses vec-only pool
- **WHEN** resident 查询发射一个 `VEC_ONLY` read 且没有可用 fixed registered vector buffer
- **THEN** 系统 SHALL 从 vec-only 专用 buffer pool 获取读取 buffer
- **AND** 该 buffer MUST 满足 vector read 的对齐和容量要求
- **AND** 查询结果语义 MUST 与通用 buffer pool 路径保持一致

#### Scenario: Generic buffer pool remains available for non-vector-only reads
- **WHEN** scheduler 发射 `VEC_ALL`、`PAYLOAD` 或 `CLUSTER_BLOCK` 请求
- **THEN** 系统 SHALL 保持现有通用 buffer ownership 和 fallback 行为
- **AND** vec-only buffer lifecycle fast path MUST NOT 改变这些请求类型的生命周期

### Requirement: Resident vector-only completions SHALL support dedicated slot cleanup
resident `VEC_ONLY` completion path SHALL 支持专用 slot cleanup/release 逻辑。该逻辑 MUST 能够准确归还 fixed vector buffer 或 vec-only pool buffer，并复用 pending slot，同时避免进入通用 cleanup path 中不必要的 buffer-pool hash lookup。

#### Scenario: Vector-only completion releases vec-only pool buffer
- **WHEN** 一个使用 vec-only pool buffer 的 `VEC_ONLY` read completion 被 dispatch
- **THEN** 系统 SHALL 在 vector 被 rerank consumer 接收后将 buffer 归还到 vec-only pool
- **AND** pending slot SHALL 被释放并可供后续请求复用
- **AND** buffer MUST NOT 被重复释放或泄漏

#### Scenario: Vector-only completion releases fixed registered buffer
- **WHEN** 一个使用 fixed registered vector buffer 的 `VEC_ONLY` read completion 被 dispatch
- **THEN** 系统 SHALL 将 fixed buffer index 归还到 fixed vector buffer free-list
- **AND** pending slot SHALL 被释放并可供后续请求复用
- **AND** io_uring completion user-data 到 pending slot 的映射语义 MUST 保持不变

### Requirement: Fixed vector buffer capacity SHALL be configurable independently of io queue depth
resident vector-only data read path SHALL support configuring fixed registered vector buffer count independently from `io_queue_depth`。默认配置 MUST preserve 当前行为；显式配置时，scheduler SHALL 尝试使用指定数量初始化 fixed vector buffers，并在不可用时保持正确 fallback。

#### Scenario: Default fixed vector buffer count preserves current behavior
- **WHEN** fixed vector buffer count 未显式配置或为 0
- **THEN** 系统 SHALL 使用与当前 `io_queue_depth` 等价的 fixed vector buffer 数量
- **AND** 现有 benchmark 配置的行为 MUST 保持兼容

#### Scenario: Explicit fixed vector buffer count changes registered buffer capacity
- **WHEN** benchmark 或查询配置显式设置 fixed vector buffer count
- **THEN** scheduler SHALL 使用该数量尝试初始化 fixed registered vector buffers
- **AND** benchmark 输出 SHALL 能够反映 fixed-buffer hit/miss 的变化

#### Scenario: Fixed buffer registration failure falls back safely
- **WHEN** requested fixed vector buffer registration fails
- **THEN** 系统 SHALL 回退到非 fixed-buffer data read path
- **AND** 查询正确性、completion dispatch 和 buffer cleanup MUST 保持正确

## ADDED Requirements (from vector-read-submit-pipeline-compaction)

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
