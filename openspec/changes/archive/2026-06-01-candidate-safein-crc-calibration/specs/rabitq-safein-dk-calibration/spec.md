## ADDED Requirements

### Requirement: SafeIn threshold SHALL support candidate-level CRC source
系统 SHALL 支持将 candidate-level CRC 校准结果作为 SafeIn 阈值来源，并与 legacy exact-L2 `d_k` 和 RabitQ kth `d_k` 校准路径共存。

#### Scenario: candidate CRC 阈值被加载为 SafeIn threshold
- **WHEN** 索引 metadata 或 sidecar calibration artifact 包含来源为 `candidate_crc` 的 SafeIn 阈值
- **THEN** runtime MUST 使用该阈值作为 SafeIn `T`
- **AND** runtime MUST 保留 legacy exact-L2 / RabitQ kth 阈值信息作为诊断或 fallback 数据

#### Scenario: candidate CRC 未启用时保持旧路径
- **WHEN** SafeIn threshold source 未配置为 `candidate_crc`
- **THEN** 系统 MUST 保持现有 exact-L2 或 RabitQ kth SafeIn `d_k` 校准行为
- **AND** 旧索引 MUST 不需要重建即可继续查询

## MODIFIED Requirements

### Requirement: SafeIn d_k provenance SHALL be persisted and observable
系统 SHALL 持久化并暴露足够的 metadata，用于识别 SafeIn 专用阈值的校准方式。metadata MUST 区分 legacy exact-L2 `d_k`、RabitQ kth `d_k` 和 candidate-level CRC threshold。

#### Scenario: metadata 记录 RabitQ SafeIn d_k 来源
- **WHEN** 索引在构建或更新时启用了 RabitQ SafeIn `d_k` 校准
- **THEN** metadata MUST 包含 SafeIn `d_k` 的值
- **AND** MUST 包含距离空间、percentile、calibration query 数、搜索范围、bits，以及在 nprobe-limited 情况下的 nprobe

#### Scenario: metadata 记录 candidate CRC threshold 来源
- **WHEN** 索引或 sidecar artifact 使用 candidate-level CRC SafeIn threshold
- **THEN** metadata MUST 包含 SafeIn threshold 的值
- **AND** MUST 包含 threshold source、目标 `beta`、calibration query 数、validation query 数、candidate domain、nprobe、top-k、bits、calibration falseSafeIn rate 和 validation falseSafeIn rate

#### Scenario: diagnostics 暴露 SafeIn threshold 来源
- **WHEN** benchmark 或 diagnostic 工具报告分类统计
- **THEN** 它 MUST 报告 SafeIn 使用的是 legacy exact-L2 `d_k`、RabitQ kth `d_k` 还是 candidate-level CRC threshold
- **AND** 它 MUST 报告 SafeIn threshold 的值以及该来源对应的校准参数
