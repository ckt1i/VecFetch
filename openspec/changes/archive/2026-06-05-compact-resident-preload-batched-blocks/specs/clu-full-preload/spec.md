## ADDED Requirements

### Requirement: Full preload SHALL expose a compact resident retention mode
`.clu` full preload 能力 SHALL 支持 compact resident retention mode。该模式仍属于 query-time cluster-data resident mode，但 preload 完成后只保留查询所需的 per-cluster resident state，而不是保留完整 `.clu` file buffer。

#### Scenario: Compact mode is selected for resident preload
- **WHEN** benchmark 或在线查询启用 compact resident preload
- **THEN** 系统 SHALL 在查询前完成所有 cluster block 的读取、解析和 resident view 构建
- **AND** 查询阶段 SHALL 不需要新的 `.clu` block read
- **AND** preload 完成后 `resident_file_buffer_bytes` SHALL 为 0

#### Scenario: Existing full-file mode remains available for comparison
- **WHEN** benchmark 需要比较旧 full-file preload 与 compact resident preload
- **THEN** 系统 SHALL 能保留或模拟旧 full-file resident 模式作为对照
- **AND** 两种模式 SHALL 输出可区分的 loading/retention mode metadata

### Requirement: Resident cluster memory accounting SHALL distinguish retained bytes from read bytes
`.clu` preload 相关统计 SHALL 区分 preload 过程中读过的字节和 preload 后仍 resident 的字节。系统 MUST NOT 只用 `.clu` 文件大小代表 compact resident 模式下的实际内存占用。

#### Scenario: Compact preload reports zero retained file buffer
- **WHEN** compact resident preload 完成并写出 benchmark 结果
- **THEN** 输出 SHALL 包含 `resident_file_buffer_bytes`
- **AND** compact 模式下该字段 SHALL 为 0
- **AND** 输出 SHALL 另外报告 actual resident cluster component bytes

#### Scenario: Full-file preload reports retained file buffer
- **WHEN** 旧 full-file preload 模式完成并写出 benchmark 结果
- **THEN** 输出 SHALL 包含完整 `.clu` file buffer retained bytes
- **AND** 该值 SHALL 可与 compact 模式的 `resident_file_buffer_bytes` 直接比较

### Requirement: Compact preload SHALL keep payload I/O outside cluster preload
compact resident preload SHALL 继续只处理 cluster-side quantized-vector 与 address-related state。系统 MUST NOT 因 compact preload 改造而把 `data.dat` 原始向量或 payload body 读入 resident cluster state。

#### Scenario: Rerank keeps reading raw vectors through the normal path
- **WHEN** compact resident preload 已完成并执行需要 rerank 的 query
- **THEN** rerank 原始向量 SHALL 继续从现有 `data.dat`/payload path 按需读取
- **AND** compact resident cluster state SHALL 不包含原始向量 body cache
