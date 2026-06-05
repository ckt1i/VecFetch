## ADDED Requirements

### Requirement: IVF-RaBitQ storage SHALL persist official bit semantics
IVF-RaBitQ index storage SHALL persist unambiguous bit semantics for official `1+n` indexes. Metadata MUST distinguish `total_bits` used for reported RaBitQ precision from `ex_bits` used for ExData storage and kernels.

#### Scenario: Official index metadata contains total and ex bits
- **WHEN** 构建器写出 official `1+n` IVF-RaBitQ index
- **THEN** metadata 必须包含 `rabitq_total_bits`
- **AND** metadata 必须包含 `rabitq_ex_bits`
- **AND** metadata 必须满足 `rabitq_total_bits == rabitq_ex_bits + 1`

#### Scenario: Legacy storage is labeled separately
- **WHEN** 构建器或 reader 处理 legacy signed-magnitude index
- **THEN** metadata 必须将其标记为 legacy signed-magnitude storage
- **AND** metadata 不得把 legacy `bits` 字段解释为 official `total_bits`

### Requirement: Official storage build SHALL keep raw vector plane decoupled
official `1+n` storage SHALL keep the existing three-plane baseline organization: compressed index, raw vector plane, and external payload backend. The new RaBitQ format MUST only change the compressed index plane.

#### Scenario: Official build emits compressed index and raw vectors separately
- **WHEN** official `1+n` index build completes
- **THEN** compressed cluster/index files 必须包含 official ExData storage
- **AND** raw vectors 必须继续存储在可由 row id addressing 的 raw vector plane
- **AND** payload bytes 不得嵌入 compressed cluster file

#### Scenario: Rebuild does not reorder raw vector plane semantics
- **WHEN** 用户为同一数据集重建 official `1+n` index
- **THEN** row id 到 raw vector file offset 的映射必须保持由 dataset row order 决定
- **AND** 不得依赖 cluster-local order 读取 raw vectors

### Requirement: Official build outputs SHALL be isolated from legacy indexes
official `1+n` index build SHALL write to a distinct output directory or format marker so that legacy and official indexes cannot be confused.

#### Scenario: Official rebuild does not overwrite legacy index
- **WHEN** 用户从 legacy signed-magnitude index 构建 official `1+n` index
- **THEN** build tool 必须写入新的 index directory 或要求显式 overwrite flag
- **AND** build metadata 必须记录 source dataset、nlist、format version and bit semantics

#### Scenario: Reader rejects incompatible format markers
- **WHEN** reader 配置为 official-only mode 但打开 legacy index
- **THEN** reader 必须返回明确格式不匹配错误
- **AND** 不得静默按 official estimator 查询 legacy payload
