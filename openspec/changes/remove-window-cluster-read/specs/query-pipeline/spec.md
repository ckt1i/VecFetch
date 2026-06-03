## MODIFIED Requirements

### Requirement: OverlapScheduler
在保持现有结果正确性和 recall 语义的前提下，`OverlapScheduler` SHALL 使用 resident full-preload cluster data 作为唯一 cluster probe 路径。系统 MUST 使用 resident query hot path、可复用 query wrapper 以及 resident 条件下的轻量候选提交组织方式；不得回退到 window cluster block read、prefetch/refill 或 query-time cluster block parse。`coarse_select_ms` MUST 继续独立于 `probe_time_ms` 统计；`probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms` 和 `probe_submit_ms` MUST 继续作为 resident query 主路径的正式输出字段。对于 padded-Hadamard 索引，查询管线 MUST 在 query 开头按 metadata 中的 `logical_dim` / `effective_dim` 规则执行 zero-padding，并使用 query-once Hadamard rotation 后的 pre-rotated prepare 路径，而不是退回随机旋转矩阵路径。

#### Scenario: 端到端小规模搜索
- **WHEN** 对一个已构建且 resident cluster views 可用的 IVF 索引执行 `Search(query_vec, top_k=10, nprobe=2)`
- **THEN** 返回的 `SearchResult` SHALL 按 distance 升序排列
- **AND** stats SHALL report probed cluster/candidate counts
- **AND** query execution SHALL NOT submit `CLUSTER_BLOCK` I/O

#### Scenario: 全部 SafeOut 时不发 raw-data I/O
- **WHEN** 一个索引的分类阈值使所有 candidate 都成为 SafeOut
- **THEN** 搜索结果 SHALL 为空或不包含 SafeOut candidate
- **AND** stats.total_io_submitted SHALL remain zero for raw vector/payload reads

#### Scenario: nprobe=1 单 cluster
- **WHEN** 一个 query 以 `nprobe=1` 运行
- **THEN** 系统 SHALL 只 probe 最近的 resident cluster
- **AND** 所有返回结果的地址来源 SHALL 属于该 probed cluster

#### Scenario: Probe 与 raw-data I/O overlap
- **WHEN** 一个 query 产生需要 raw vector 或 payload 读取的 candidate
- **THEN** scheduler SHALL preserve vector/payload submit、completion dispatch、rerank 和 final drain 语义
- **AND** cluster-side resident probe SHALL NOT depend on window refill state

#### Scenario: Resident hot path exposes query-path timings
- **WHEN** 查询使用 resident full-preload 主路径
- **THEN** 搜索统计 MUST 独立输出 `coarse_select_ms`
- **AND** MUST 独立输出 `probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms` 和 `probe_submit_ms`
- **AND** `probe_time_ms` MUST 保持为这些 probe 子阶段的兼容聚合字段，而不包含 `coarse_select_ms`

#### Scenario: Resident single-assignment serving may use lightweight submit organization
- **WHEN** resident 查询路径满足 single-assignment 条件
- **THEN** 系统 MAY 使用 resident 专用轻量候选提交组织方式
- **AND** 最终 recall 语义与结果排序 MUST 与参考路径保持一致

#### Scenario: Padded-Hadamard query uses query-once padded rotation
- **WHEN** 查询运行在 `logical_dim` 非 2 的幂且 metadata 标记为 padded-Hadamard 的索引上
- **THEN** query wrapper MUST 先把 query 从 `logical_dim` zero-pad 到 `effective_dim`
- **AND** MUST 在 query 开头只执行一次 Hadamard rotation
- **AND** probe 期间 MUST 复用 pre-rotated query path

## ADDED Requirements

### Requirement: Query pipeline SHALL require resident cluster availability before probing
The query pipeline MUST verify resident cluster availability before any cluster probe begins. If resident cluster data is unavailable, the system MUST either perform an explicit preload outside the measured query phase or fail clearly; it MUST NOT fall back to query-time window cluster reads.

#### Scenario: Missing resident state does not trigger window fallback
- **WHEN** a scheduler is invoked without resident cluster views
- **THEN** the system SHALL NOT call window `SubmitClusterRead` or equivalent per-cluster read logic
- **AND** it SHALL make the missing preload condition explicit through preload or error handling
