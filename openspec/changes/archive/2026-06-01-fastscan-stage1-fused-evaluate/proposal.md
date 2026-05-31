## Why

在 `mask-aware-stage2-kernel` 之后，Stage2 热点已经被压低，`RaBitQEstimator::EstimateDistanceFastScan` 成为与 masked Stage2 同量级的查询 CPU 热点。当前 Stage1 对同一个 FastScan block 先写 `dists[32]`，再分别读取 `dists` 和 `block_norms` 计算 SafeOut/SafeIn mask，存在可融合的重复内存访问和循环开销。

当前代码已经合入 `safe-boundary-error-frontier` 的区间式 SafeIn/SafeOut 语义。Stage1 fused evaluate 必须对齐当前 oracle：

- SafeOut: `dist > safeout_frontier_upper + safeout_margin_factor * norm`
- SafeIn: `dist < safein_d_k - safein_margin_factor * norm`

不得恢复旧的 `dynamic_d_k + 2 * margin` / `safein_d_k - 2 * margin` 口径。

本变更目标是在不调整 epsilon/nprobe 等搜索参数、不改变 recall/top-k 语义的前提下，融合 Stage1 的 dequantize 与 SafeOut/SafeIn mask 计算，降低 `ProbeCluster` 的 Stage1 CPU 成本。

## What Changes

- 新增 FastScan Stage1 fused evaluate API：在一次 SIMD pass 中完成 `raw_accu -> dist`、SafeOut mask 和可选 SafeIn mask 计算。
- `ClusterProber::Probe` 在 Stage1 热路径使用 fused API，保留现有三步路径作为测试 oracle 或 fallback。
- fused API 仍输出 `dists[32]`，保证 Stage2、candidate emit、统计和后续逻辑不需要改变。
- fused API 使用 `safeout_frontier_upper` 和单边 margin 语义，保持与 `FastScanSafeOutMask` / `FastScanSafeInMask` 当前实现逐 lane 一致。
- 保持现有 `AccumulateBlock` 路径不变；本轮不实现 `n * 32` / `n * 64` 维度模板化。
- 保持现有 benchmark 字段名；可追加 Stage1 fused-path 诊断字段，但不得删除旧字段。
- 不做参数优化，不改变 epsilon、nprobe、early-stop、two-level routing、CRC 或 rerank 行为。

## Capabilities

### New Capabilities
- `fastscan-stage1-fused-evaluate`: FastScan Stage1 能在一次 pass 中融合 dequantize 与 SafeOut/SafeIn mask 计算，并保持与旧三步路径数值和分类一致。

### Modified Capabilities

## Impact

- 影响代码：
  - `include/vdb/simd/fastscan.h`
  - `src/simd/fastscan.cpp`
  - `include/vdb/rabitq/rabitq_estimator.h`
  - `src/rabitq/rabitq_estimator.cpp`
  - `src/index/cluster_prober.cpp`
  - `include/vdb/index/cluster_prober.h`
  - `include/vdb/query/search_context.h`
  - `benchmarks/bench_e2e.cpp`
  - 相关 SIMD、RaBitQ estimator、ClusterProber/OverlapScheduler 测试
- 不改变索引文件格式。
- 不改变查询结果排序语义、recall 计算口径或 benchmark 默认参数。
- 主要风险是 fused path 与旧 `FastScanDequantize + FastScanSafeOutMask + FastScanSafeInMask` 的边界条件不一致，需要以单元测试和端到端真实 recall 验证兜底。
- 依赖当前 `safe-boundary-error-frontier` 语义：`CandidateBatch.est_error`、CRC estimate heap 的 `error_bound`、`safeout_frontier_upper` 输入契约必须保持不变。
