## Why

现有 SafeIn 阈值主要从 query-level 的 `d_k` 分位数得到，但当前实验显示主要问题是 candidate-level 的 `falseSafeIn / SafeIn` 比例，而不是单个查询是否出现 `d_k` 校准失败。需要新增一个直接以候选级 false ratio 为目标的离线校准方法，使线上 SafeIn 判定和实际优化目标一致。

## What Changes

- 新增 candidate-level SafeIn CRC 校准流程：离线 replay 线上候选流，按 `U_i = d_hat_i + e_i` 统计 SafeIn 与 falseSafeIn，并选择满足 `sum false_i(T) / max(sum SafeIn_i(T), 1) <= beta` 的阈值 `T`。
- 线上 SafeIn 比较直接使用 `U_i < T`，其中 `U_i = d_hat_i + e_i`，不额外引入 query-local top-k frontier 或原始向量 `d_k` 比较。
- 离线校准仍使用 full-vector exact top-k 作为 falseSafeIn 标签来源，但 SafeIn 阈值不再从 exact `r_k(q)` 的分位数直接取值。
- 校准流程必须支持 calibration/validation 分离：calibration 集选择阈值，held-out validation 集报告实际 `falseSafeIn / SafeIn`、SafeIn 数量和阶段分布。
- 保留已有 exact-L2 / RabitQ kth SafeIn `d_k` 校准能力作为 legacy/reference 路径，不做破坏性移除。

## Capabilities

### New Capabilities
- `candidate-safein-crc-calibration`: 定义候选级 SafeIn CRC replay 校准、阈值选择、验证指标和运行时比较语义。

### Modified Capabilities
- `rabitq-safein-dk-calibration`: 增加 candidate-level CRC 校准作为新的 SafeIn 阈值来源，并要求 provenance 区分 legacy exact/RabitQ kth 与 candidate-level CRC。

## Impact

- Affected calibration code: SafeIn 阈值生成、候选 replay、exact top-k label 读取和 calibration/validation split。
- Affected runtime query path: SafeIn 判定继续消费单个全局阈值，但阈值来源可变；线上比较保持 `d_hat_i + e_i < T`。
- Affected benchmarks: `bench_vector_search` / 相关 E2E benchmark 需要能够加载新阈值并报告 candidate-level `falseSafeIn / SafeIn`、Stage1/Stage2 SafeIn、falseSafeIn 和 Uncertain。
- Affected metadata: index 或 sidecar calibration artifact 需要记录阈值、目标 `beta`、候选域、nprobe、校准样本数、验证结果和阈值来源。
