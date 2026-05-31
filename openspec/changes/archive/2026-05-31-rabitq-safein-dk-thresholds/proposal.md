## Why

当前 ConANN 的分类把同一个校准得到的 `d_k` 同时用于 SafeIn 确认，以及动态 SafeOut 的静态下界回退。离线诊断表明，基于真实 L2 校准得到的 `d_k` 与 SafeIn 实际使用的多 bit RabitQ Stage2 距离空间并不匹配，而 SafeOut 本身已经有更强的 query-time estimated kth frontier，不应继续被 SafeIn 的校准阈值约束。

## What Changes

- 引入一个专用于 SafeIn 的 `d_k`，并在 multi-bit RabitQ Stage2 距离空间中进行校准。
- SafeOut 的阈值只依赖 query-time estimated kth distance 加 margin，并在 estimate heap 充满后生效。
- 保留旧索引兼容性，仍能读取只包含现有 exact-L2 `d_k` 的索引。
- 增加 benchmark / diagnostic 支持，用于对比 exact-L2 与 RabitQ-space 的 SafeIn `d_k` 校准结果，并记录校准来源。
- 不改变最终精确 rerank 语义，也不改变最终 top-k 排序结果。

## Capabilities

### New Capabilities

- `rabitq-safein-dk-calibration`: 覆盖 build-time 和离线的 SafeIn 专用 `d_k` 校准能力，基于 multi-bit RabitQ Stage2 距离空间。

### Modified Capabilities

- `dynamic-safeout`: SafeOut 分类必须使用 query-time estimated kth threshold，且在该变更启用后不再把 static `d_k` 作为回退下界。

## Impact

- 受影响的 build / index 代码：`IvfBuilder`、`ConANN`、segment metadata 的序列化/加载，以及配置校准查询的 benchmark 构建路径。
- 受影响的 query 代码：`ConANN::ClassifyAdaptive`、`ClusterProber`、`OverlapScheduler::ProbeCluster`，以及所有计算 `dynamic_d_k` 的路径。
- 受影响的诊断工具：RabitQ accuracy / diagnostic benchmark 应该暴露 SafeIn `d_k` 的空间、percentile、采样数，以及 SafeOut 阈值模式。
- 兼容性：没有 SafeIn 专用 RabitQ `d_k` 的老索引必须继续可加载，并使用 legacy exact-L2 `d_k` 作为 SafeIn 回退。
