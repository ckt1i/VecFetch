## ADDED Requirements

### Requirement: Candidate-level SafeIn CRC calibration SHALL replay serving candidates
系统 SHALL 提供 candidate-level SafeIn CRC 校准流程，用于按线上候选域 replay SafeIn 判定并生成候选级校准记录。每条记录 MUST 至少包含 candidate id、query id、stage、`U_i = d_hat_i + e_i`、以及该 candidate 是否属于 full-vector exact top-k。

#### Scenario: replay 使用线上候选域
- **WHEN** candidate-level SafeIn CRC 校准以 `nprobe` 候选域运行
- **THEN** 系统 MUST 使用与线上配置一致的聚类选择和 nprobe 生成候选流
- **AND** 系统 MUST 为被访问 candidate 计算与线上 SafeIn 路径一致的 `d_hat_i` 和 `e_i`

#### Scenario: replay 使用 exact top-k 作为 falseSafeIn 标签
- **WHEN** candidate-level SafeIn CRC 校准生成候选级记录
- **THEN** 系统 MUST 使用 full-vector exact top-k id 集合标记 candidate 是否为 true top-k
- **AND** 系统 MUST NOT 使用 exact `r_k(q)` 分位数作为该模式的 SafeIn 阈值来源

### Requirement: Candidate-level SafeIn CRC calibration SHALL choose threshold by falseSafeIn ratio
系统 SHALL 从 candidate-level replay 记录中选择全局 SafeIn 阈值 `T`，使 calibration 集上 candidate-level `falseSafeIn / SafeIn` 不超过目标 `beta`，并在合法阈值中最大化 SafeIn 数量。
这里的 `beta` MUST 表示条件比例 `falseSafeIn / SafeIn`，而不是 `falseSafeIn / all_candidates`。

#### Scenario: 阈值选择满足 beta
- **WHEN** calibration records 已按 `U_i` 排序并配置目标 `beta`
- **THEN** 系统 MUST 选择一个阈值 `T`，使 `sum false_i(T) / max(sum SafeIn_i(T), 1) <= beta`
- **AND** `SafeIn_i(T)` MUST 按 `U_i < T` 计算

#### Scenario: 多个阈值满足 beta
- **WHEN** 多个候选阈值都满足 `falseSafeIn / SafeIn <= beta`
- **THEN** 系统 MUST 选择 SafeIn 数量最多的阈值
- **AND** 若 SafeIn 数量相同，系统 MUST 选择更保守的较小阈值

#### Scenario: 没有非空 SafeIn 阈值满足 beta
- **WHEN** 所有会产生 SafeIn 的候选阈值都违反 `beta`
- **THEN** 系统 MUST 返回一个不会产生 SafeIn 的保守阈值
- **AND** 校准结果 MUST 明确报告该 run 没有找到非空合法阈值

### Requirement: Candidate-level SafeIn CRC validation SHALL use a held-out query split
系统 SHALL 将 candidate-level SafeIn CRC 的阈值选择和效果报告分离。calibration split 用于选择 `T`，held-out validation split 用于报告实际候选级效果。

#### Scenario: validation 报告候选级指标
- **WHEN** candidate-level SafeIn CRC 校准完成
- **THEN** validation 输出 MUST 包含 `safein_count`、`false_safein_count`、`false_safein_rate` 和 `uncertain_count`
- **AND** validation 输出 MUST 分别报告 Stage1、Stage2/final 的 SafeIn 与 falseSafeIn 统计
- **AND** validation 输出 SHOULD 包含 query-block bootstrap 95% CI 用于表示 `falseSafeIn / SafeIn` 的采样不确定性

#### Scenario: validation 记录 split provenance
- **WHEN** validation 结果被写入 JSON、metadata 或 sidecar artifact
- **THEN** 输出 MUST 记录 calibration query 数、validation query 数、split seed、candidate domain、nprobe、top-k、bits 和目标 `beta`

### Requirement: Runtime SafeIn comparison SHALL use candidate upper bound only
线上查询路径 SHALL 使用 candidate-level CRC 校准得到的全局阈值 `T`，并直接以 `U_i = d_hat_i + e_i` 进行 SafeIn 比较。

#### Scenario: runtime 使用 candidate CRC 阈值
- **WHEN** runtime 加载到来源为 `candidate_crc` 的 SafeIn 阈值
- **THEN** candidate MUST 仅在 `d_hat_i + e_i < T` 时被分类为 SafeIn
- **AND** runtime MUST NOT 在该比较中使用 full-vector exact distance、exact `r_k(q)` 或 query-local top-k frontier

#### Scenario: Stage1 和 Stage2 使用同一上界语义
- **WHEN** Stage1 或 Stage2 执行 SafeIn 分类
- **THEN** 两个阶段 MUST 使用与各自 estimator 对应的 `d_hat_i` 和 `e_i` 计算 `U_i`
- **AND** 两个阶段 MUST 使用相同的 candidate upper-bound 比较语义 `U_i < T`
