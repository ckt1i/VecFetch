## Why

当前 SafeIn/SafeOut 分类使用 `d_hat +- 2 * margin`，这里隐含地把“当前候选自己的误差界”同时用于候选本身和当前 top-k frontier。由于不同候选的 residual norm 不同，而 query-time top-k frontier 也有它自己的误差界，这个假设并不成立。

这次变更要把分类边界显式化：SafeOut 用候选下界去比较一个保守的 query-time top-k 上界 frontier；SafeIn 用候选上界去比较一个基于原始向量精确距离校准得到的 SafeIn 接受阈值。

## What Changes

- 在 CRC / estimate heap 使用的 RaBitQ 估计距离旁边，同时跟踪每个候选的估计误差。
- 将 dynamic SafeOut 的阈值从 `dynamic_d_k + 2 * candidate_error` 改为更保守的 top-k 上界 frontier：
  `safeout_frontier_upper = kth(d_hat) + max_error_in_est_heap`。
- 将 SafeOut 判定改为 `d_hat_i - e_i > safeout_frontier_upper`。
- 将 SafeIn 判定改为基于原始向量精确距离校准的 `d_hat_i + e_i < d_k_fullvector_safein`。
- 保持 heap 未满时的保守行为：只有 estimate heap 至少包含 `top_k` 个条目后，才允许 estimate-driven SafeOut。
- 保留 FastScan estimate kernel 的优化边界；本次只调整分类 mask 的阈值，不改变 single-bit 距离估计 kernel 可独立优化这一点。
- 增加 COCO100k 仅向量搜索验证：使用 `bench_vector_search.cpp`，配置 `nlist=2048`、`nprobe=64`，报告新逻辑下 Stage1 / Stage2 的 SafeIn / SafeOut / Uncertain 数量。

## Capabilities

### New Capabilities

- 无。

### Modified Capabilities

- `dynamic-safeout`：将 dynamic SafeOut 从“kth estimate 加候选双倍 margin”的规则，改为“候选下界对保守 top-k 上界 frontier”的规则，并将 SafeIn 对齐到基于原始向量精确距离校准的接受阈值。

## Impact

- 查询分类热路径：`ConANN::ClassifyAdaptive`、`ClusterProber`、Stage1 FastScan mask helper、Stage2 SIMD 分类，以及 `OverlapScheduler` 的 estimate heap 维护。
- 运行时状态：estimate heap 条目需要同时携带 `d_hat` 和候选误差界；query 侧需要维护导出的 `max_error_in_est_heap`。
- 当前仍假设 `dynamic_d_k + 2 * margin` 的诊断 / replay 工具，需要暴露或适配新的 frontier 公式。
- Benchmark：需要在 COCO100k 上用 `nlist=2048`、`nprobe=64` 重跑 `bench_vector_search.cpp`，检查两阶段 SafeIn / SafeOut / Uncertain 的分布。
