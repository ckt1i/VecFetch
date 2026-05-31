## ADDED Requirements

### Requirement: SafeIn d_k SHALL support RabitQ Stage2 calibration
系统 SHALL 支持在 multi-bit RabitQ Stage2 距离空间中校准一个专用于 SafeIn 的 `d_k`。这个校准值 SHALL 与 legacy exact-L2 `d_k` 分离。

#### Scenario: 从 RabitQ Stage2 估计值校准 SafeIn d_k
- **WHEN** 为索引构建或离线校准启用了 SafeIn RabitQ 校准
- **THEN** 系统 MUST 使用 multi-bit RabitQ Stage2 estimator 计算每个候选的 `est_dist_s2`
- **AND** 系统 MUST 从配置的候选域中取每个 calibration query 的 kth-smallest `est_dist_s2`
- **AND** 系统 MUST 使用配置的低分位规则聚合这些 query-level kth 值

#### Scenario: legacy exact d_k 保留可用
- **WHEN** SafeIn RabitQ 校准已启用
- **THEN** 系统 MUST 保留现有 exact-L2 `d_k` 作为 legacy / reference 校准数据
- **AND** 系统 MUST NOT 用 RabitQ-space 的 SafeIn 阈值覆盖它

### Requirement: SafeIn d_k calibration SHALL support full and nprobe candidate domains
SafeIn RabitQ `d_k` 校准 SHALL 同时支持 full-database 候选评估和 nprobe-limited serving 域评估。

#### Scenario: full 候选域校准
- **WHEN** 校准搜索范围为 `full`
- **THEN** 每个 calibration query MUST 针对索引中的所有数据库向量评估 Stage2 RabitQ 距离

#### Scenario: nprobe 候选域校准
- **WHEN** 校准搜索范围为 `nprobe`
- **THEN** 每个 calibration query MUST 先选择配置的最近聚类数
- **AND** 系统 MUST 只对这些聚类中的向量评估 Stage2 RabitQ 距离

### Requirement: SafeIn d_k provenance SHALL be persisted and observable
系统 SHALL 持久化并暴露足够的 metadata，用于识别 SafeIn 专用 `d_k` 的校准方式。

#### Scenario: metadata 记录 RabitQ SafeIn d_k 来源
- **WHEN** 索引在构建或更新时启用了 RabitQ SafeIn `d_k` 校准
- **THEN** metadata MUST 包含 SafeIn `d_k` 的值
- **AND** MUST 包含距离空间、percentile、calibration query 数、搜索范围、bits，以及在 nprobe-limited 情况下的 nprobe

#### Scenario: diagnostics 暴露 SafeIn d_k 来源
- **WHEN** benchmark 或 diagnostic 工具报告分类统计
- **THEN** 它 MUST 报告 SafeIn 使用的是 legacy exact-L2 `d_k` 还是 RabitQ Stage2 `d_k`
- **AND** 它 MUST 报告 SafeIn `d_k` 的值和校准 percentile

### Requirement: Runtime SHALL fall back for old indexes
运行时索引加载 SHALL 继续兼容那些没有 SafeIn 专用 RabitQ `d_k` metadata 的索引。

#### Scenario: 旧索引没有 SafeIn d_k metadata
- **WHEN** 索引缺少 SafeIn 专用 `d_k` metadata
- **THEN** runtime MUST 使用 legacy exact-L2 `d_k` 作为 SafeIn 阈值
- **AND** 查询执行 MUST 不需要重建索引

#### Scenario: 新索引带有 SafeIn d_k metadata
- **WHEN** 索引包含 SafeIn 专用 RabitQ `d_k` metadata
- **THEN** runtime MUST 使用该值进行 SafeIn 分类
- **AND** 它 MUST 保留 legacy exact-L2 `d_k` 以便兼容和诊断
