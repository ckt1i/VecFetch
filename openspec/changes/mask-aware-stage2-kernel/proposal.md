## Why

现有 `ProbeCluster` 的 Stage2 ExRaBitQ 路径已经成为主要 CPU 热点。当前 compact block kernel 按 `valid_count` 计算整块 lane，但实际需要 Stage2 复算的 lane 只由 `lane_mask` 中的 uncertain candidate 决定，导致在稀疏 uncertain 场景下做了大量无效 SIMD 工作。

本变更目标是在不调整 epsilon/nprobe 等参数、不改变 recall/top-k 语义的前提下，让 Stage2 kernel 只计算需要的 lane，降低 `ProbeCluster` 的 CPU 时间。

## What Changes

- 新增 mask-aware Stage2 ExRaBitQ compact kernel，输入 `lane_mask`，只对 mask 中的 lane 计算 IP。
- `ClusterProber::Probe` 在 compact v11 路径优先调用 mask-aware kernel，原 full-lane kernel 保留为 fallback 和对照。
- 增加 Stage2 lane 利用率统计，用于验证每个 Stage2 block 中实际 uncertain lane 密度和跳过计算量。
- 保留现有 `fine-grained-timing` 字段；新增统计不改变搜索结果和现有 JSON 字段名。
- 不做参数优化，不改变 epsilon、nprobe、early-stop、two-level routing 默认行为。
- 不实现 768-only SIMD 特化；后续如需模板化，应面向 `n * 32` 或 `n * 64` 维度的通用路径。

## Capabilities

### New Capabilities
- `stage2-mask-aware-kernel`: Stage2 ExRaBitQ compact kernel 能基于 uncertain lane mask 跳过无关 lane，并暴露必要统计用于性能归因。

### Modified Capabilities

## Impact

- 影响代码：
  - `src/index/cluster_prober.cpp`
  - `include/vdb/index/cluster_prober.h`
  - `src/simd/ip_exrabitq.cpp`
  - `include/vdb/simd/ip_exrabitq.h`
  - `include/vdb/query/search_context.h`
  - `benchmarks/bench_e2e.cpp`
  - 相关 SIMD / cluster prober 测试
- 不改变索引文件格式。
- 不改变查询 API、top-k 排序语义、recall 计算口径或 benchmark 默认参数。
- 主要风险是 mask-aware kernel 与现有 full-lane kernel 的数值一致性，需要用单元测试和端到端 recall 验证兜底。
