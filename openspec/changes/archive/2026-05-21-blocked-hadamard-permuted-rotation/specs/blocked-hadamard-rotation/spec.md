## ADDED Requirements

### Requirement: Builder 支持非幂次维度下的 blocked Hadamard permuted 构建
系统 SHALL 支持非 2 的幂维度使用 `blocked_hadamard_permuted` 旋转模式。在该模式下，Builder SHALL 保持 `logical_dim == effective_dim`，SHALL 依据逻辑维度生成确定性的 2 的幂分块，SHALL 使用固定种子的维度置换，并 SHALL 构建仅由“置换 + 每块 Hadamard + 每块符号对角”组成的 hot-path 变换。

#### Scenario: 无 padding 的 MSMARCO 768d 构建
- **WHEN** 在 `768` 维数据上配置 `blocked_hadamard_permuted`
- **THEN** 输出索引满足 `logical_dim = 768`、`effective_dim = 768`，且分块方案等价于 `512 + 256`

#### Scenario: 热路径不回退到密集旋转
- **WHEN** Builder 选择 `blocked_hadamard_permuted`
- **THEN** 查询和编码阶段的旋转语义由“置换 + 分块 FWHT”定义，而不是通过扩展到更大维度的 padding，或在 hot path 使用密集 `O(D^2)` 旋转

### Requirement: Blocked Hadamard 旋转可持久化并可重开复现
系统 SHALL 持久化 `blocked_hadamard_permuted` 所需元数据，包括旋转类型、分块规模、置换规则、以及 fast path 使用的符号对角信息，保证索引重开时无歧义。重开后的索引 SHALL 重建与构建时一致的旋转语义。

#### Scenario: 重开保持 blocked 语义
- **WHEN** 从磁盘重开一个 `blocked_hadamard_permuted` 索引
- **THEN** 重开阶段读取到的旋转元数据与构建阶段一致，并标识为 `blocked_hadamard_permuted`

#### Scenario: 种子可复现
- **WHEN** 使用相同维度与同一旋转种子两次构建 `blocked_hadamard_permuted`
- **THEN** 两次构建的置换与分块定义 SHALL 一致

### Requirement: Blocked Hadamard 参与旋转查询快路径
系统 SHALL 将 `blocked_hadamard_permuted` 认定为可预旋转查询模式。构建阶段 SHALL 预计算并持久化 `rotated_centroids.bin`，搜索阶段 SHALL 对查询仅旋转一次，并使用 `PrepareQueryRotatedInto` 与已存储的旋转质心，而非每簇重复旋转。

#### Scenario: 仅一次查询旋转
- **WHEN** 搜索目标为 `blocked_hadamard_permuted` 索引
- **THEN** 查询流水线只执行一次旋转，并在簇准备阶段复用旋转后的质心

#### Scenario: 线性旋转残差语义保持
- **WHEN** `PrepareQueryRotatedInto` 接收 `rotated_q` 与 blocked-Hadamard 的 `rotated_centroid`
- **THEN** 准备出的残差等价于同一正交 blocked 旋转作用于 `(query - centroid)`
