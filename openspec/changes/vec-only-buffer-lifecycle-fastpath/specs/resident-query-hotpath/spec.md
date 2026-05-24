## ADDED Requirements

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
