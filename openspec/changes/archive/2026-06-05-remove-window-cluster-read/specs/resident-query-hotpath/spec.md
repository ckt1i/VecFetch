## MODIFIED Requirements

### Requirement: Resident query path SHALL use a reusable thin query wrapper
系统 MUST 默认进入 resident 专用 query hot path。该路径 MUST 使用可复用的 query wrapper / scratch 上下文来承载 query 期间的临时状态，并将 query 无关或容量相关的结构预先保留到 prepare/init 阶段，而不是在每个 query 内重复构造。

#### Scenario: Resident hot path is selected by default
- **WHEN** benchmark 或在线查询执行主搜索
- **THEN** 查询实现 MUST 走 resident 专用 hot path
- **AND** 它 MUST 不再回退到仅为通用 cluster window 读取路径设计的厚包装流程

#### Scenario: Query wrapper is reused across resident queries
- **WHEN** 同一线程或同一调度上下文连续执行多个 resident query
- **THEN** query wrapper / scratch 容器 MUST 复用既有容量和布局
- **AND** 每次 query 只覆盖本次所需内容，而不是重新构造整套中间对象

### Requirement: Resident single-assignment submit path SHALL support lightweight candidate organization
在 resident 且 single-assignment 的条件下，候选提交路径 MUST 支持轻量提交组织方式，以减少保守的全局去重、临时对象分配和高频小粒度提交带来的 CPU 开销。该轻量路径 MUST 保持候选语义与结果正确性不变。

#### Scenario: Lightweight submit path is used in resident single-assignment mode
- **WHEN** 查询路径满足 resident 且 single-assignment 条件
- **THEN** 系统 MUST 允许使用轻量的候选提交组织方式
- **AND** 该方式 MUST 维持与现有路径一致的候选有效性语义

#### Scenario: Fallback remains available for non-single-assignment behavior
- **WHEN** 查询路径不满足 single-assignment 条件
- **THEN** 系统 MUST 可以继续使用保守的通用候选提交组织方式
- **AND** resident 轻量路径 MUST 不强制外溢到不满足语义前提的 serving 模式

## ADDED Requirements

### Requirement: Resident query hot path SHALL not emit cluster-block requests
resident query hot path MUST NOT emit query-time `CLUSTER_BLOCK` requests. Cluster-side code and address metadata MUST come from resident cluster views, while `VEC_ONLY`、`VEC_ALL` 和 `PAYLOAD` requests remain available for raw vector and payload access.

#### Scenario: Cluster block request type is absent from query completions
- **WHEN** a resident query runs through probe, candidate submit, rerank and final drain
- **THEN** completion dispatch SHALL NOT receive or process `CLUSTER_BLOCK` query completions
- **AND** vector and payload completion behavior SHALL remain correct

#### Scenario: Non-vector request paths exclude cluster-block lifecycle
- **WHEN** scheduler emits non-vector raw-data requests
- **THEN** those requests SHALL be limited to all-read or payload request semantics
- **AND** cluster block buffer ownership SHALL NOT be part of the online query pending state
