## MODIFIED Requirements

### Requirement: OverlapScheduler
在保持现有结果正确性和 recall 语义的前提下，`OverlapScheduler` SHALL 使用 resident full-preload cluster data 和 single-assignment index 作为正式查询主路径。系统 MUST 使用 resident query hot path、可复用 query wrapper 以及 resident single-assignment 条件下的轻量候选提交组织方式；不得回退到 HNSW coarse routing、redundant assignment serving、RAIR/overlap serving、padded Hadamard 或 blocked Hadamard 查询分支。`coarse_select_ms` MUST 继续独立于 `probe_time_ms` 统计；`probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms` 和 `probe_submit_ms` MUST 继续作为 resident query 主路径的正式输出字段。对于非 2 的幂维度索引，查询管线 MUST 使用 metadata 中的 `fht_kac_rotator` 语义执行 query-once rotation 和 pre-rotated prepare path。

#### Scenario: 端到端小规模搜索
- **WHEN** 对一个已构建且 resident cluster views 可用的 single-assignment IVF 索引执行 `Search(query_vec, top_k=10, nprobe=2)`
- **THEN** 返回的 `SearchResult` SHALL 按 distance 升序排列
- **AND** stats SHALL report probed cluster/candidate counts

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
- **AND** cluster-side resident probe SHALL NOT depend on legacy routing or overlap state

#### Scenario: Resident hot path exposes query-path timings
- **WHEN** 查询使用 resident full-preload 主路径
- **THEN** 搜索统计 MUST 独立输出 `coarse_select_ms`
- **AND** MUST 独立输出 `probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms` 和 `probe_submit_ms`
- **AND** `probe_time_ms` MUST 保持为这些 probe 子阶段的兼容聚合字段，而不包含 `coarse_select_ms`

#### Scenario: Resident serving requires single assignment
- **WHEN** resident 查询路径打开一个 index
- **THEN** 系统 SHALL verify that the index is single-assignment
- **AND** redundant top-2、RAIR 或 overlap index SHALL be rejected or marked legacy unsupported

#### Scenario: FHT-Kac query uses query-once rotated path
- **WHEN** 查询运行在 `logical_dim` 非 2 的幂且 metadata 标记为 `fht_kac_rotator` 的索引上
- **THEN** query wrapper MUST 在 query 开头只执行一次 FHT-Kac rotation
- **AND** probe 期间 MUST 复用 pre-rotated query path

#### Scenario: Legacy padded or blocked rotation is not served silently
- **WHEN** 查询尝试使用 `hadamard_padded` 或 `blocked_hadamard_permuted` 索引作为正式主路径
- **THEN** 系统 SHALL fail clearly or mark the index as legacy unsupported
- **AND** it SHALL NOT silently reinterpret the rotation as random matrix or FHT-Kac

## ADDED Requirements

### Requirement: Query pipeline SHALL reject removed legacy search modes
The query pipeline SHALL reject removed legacy search modes before executing a measured search, rather than silently changing behavior.

#### Scenario: Removed HNSW routing is requested
- **WHEN** query configuration requests HNSW coarse routing
- **THEN** the query path SHALL reject the configuration or ignore it only through an explicitly documented compatibility path
- **AND** it SHALL NOT build an HNSW centroid graph

#### Scenario: Removed assignment mode is detected
- **WHEN** query metadata indicates redundant, RAIR, or overlap assignment
- **THEN** the query path SHALL fail clearly before resident single-assignment assumptions are applied
