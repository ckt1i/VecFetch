## MODIFIED Requirements

### Requirement: OverlapScheduler
在保持现有结果正确性和 recall 语义的前提下，`OverlapScheduler` SHALL 支持 resident 专用查询路径。当 `clu_read_mode=full_preload` 且 `use_resident_clusters=1` 时，系统 MUST 允许使用 resident query hot path、可复用 query wrapper 以及 resident 条件下的轻量候选提交组织方式。`coarse_select_ms` MUST 继续独立于 `probe_time_ms` 统计；`probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms` 和 `probe_submit_ms` MUST 继续作为 resident query 主路径的正式输出字段。对于 padded-Hadamard 索引，查询管线 MUST 在 query 开头按 metadata 中的 `logical_dim` / `effective_dim` 规则执行 zero-padding，并使用 query-once Hadamard rotation 后的 pre-rotated prepare 路径，而不是退回随机旋转矩阵路径。对于 exact rerank 和任何原始向量读取路径，系统 MUST 使用 raw vector plane 中的 `logical_dim` 向量，而不是要求 `data.dat` 提供 `effective_dim` 的 padded raw vectors。

#### Scenario: 端到端小规模搜索
- **WHEN** 一个已构建的 IVF 索引（N=1000, dim=128, nlist=4）被搜索
- **THEN** 搜索返回按 distance 升序排列的结果
- **AND** `stats.total_probed` 大于 0
- **AND** `stats.total_io_submitted` 大于 0

#### Scenario: 全部 SafeOut 时不发 I/O
- **WHEN** 一个索引在给定 epsilon 下所有 candidate 都 SafeOut
- **THEN** 查询返回 0 个结果
- **AND** `stats.total_io_submitted == 0`

#### Scenario: Resident hot path exposes query-path timings
- **WHEN** 查询使用 `full_preload + use_resident_clusters=1`
- **THEN** 搜索统计 MUST 独立输出 `coarse_select_ms`
- **AND** MUST 独立输出 `probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms` 和 `probe_submit_ms`
- **AND** `probe_time_ms` MUST 保持为这些 probe 子阶段的兼容聚合字段，而不包含 `coarse_select_ms`

#### Scenario: Padded-Hadamard query uses logical-dimension exact reads
- **WHEN** 查询运行在 `logical_dim` 非 2 的幂且 metadata 标记为 padded-Hadamard 的索引上
- **THEN** query wrapper MUST 先把 query 从 `logical_dim` zero-pad 到 `effective_dim`
- **AND** MUST 在 query 开头只执行一次 Hadamard rotation
- **AND** probe 期间 MUST 复用 pre-rotated query path
- **AND** rerank 或其他原始向量读取阶段 MUST 使用 `logical_dim` raw vectors
