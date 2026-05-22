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
