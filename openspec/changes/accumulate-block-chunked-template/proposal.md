## Why

当前 resident 主工作点下，FastScan Stage1 仍是查询 CPU 热点之一，其中 `AccumulateBlock` 位于 `EstimateDistanceFastScan` / Stage1 evaluate 的核心估计循环。现有实现按 SIMD 最小步长循环处理维度，后续如果只对 `512/768/1024` 写固定模板，会把优化绑定到少数数据集维度，不利于覆盖 512、768、1024 之外的 `n*32` 或 `n*64` 工作点。

本 change 目标是在不改变 FastScan 语义、packed code/LUT 布局和 query pipeline 统计口径的前提下，为 `AccumulateBlock` 增加通用 `n*32` / `n*64` chunk 模板化快路径，降低 Stage1 accumulate 热循环开销，并保留 generic fallback。

## What Changes

- 为 `simd::AccumulateBlock` 增加内部 chunked specialization：
  - `n*64` 维度优先进入 64-dim chunk fast path。
  - `n*32` 但非 `n*64` 维度进入 32-dim chunk fast path。
  - 其他合法维度继续走现有 generic 路径。
- 在 AVX512 路径中抽取固定 16-dim step，并用 `AccumulateBlockChunked<32/64>` 组合 step。
- 在 AVX2 路径中抽取固定 8-dim step，并用 `AccumulateBlockChunked<32/64>` 组合 step。
- 保持 public API、FastScan result、recall/top-k、benchmark JSON schema 不变。
- 增加覆盖 `n*32`、`n*64` 和 fallback 维度的 correctness 测试。
- 使用真实 GT 和当前分层 IVF baseline 验证端到端速度与 perf 热点变化。

## Capabilities

### New Capabilities

无。该 change 是现有 FastScan Stage1 内部 SIMD kernel 的实现优化，不引入新的外部能力。

### Modified Capabilities

- `fastscan-stage1-optimization`: 将 `EstimateDistanceFastScan` 的主估计循环优化边界扩展到 `AccumulateBlock`，要求支持维度无关的 `n*32` / `n*64` chunked fast path，并保持 fallback 与结果等价。

## Impact

- 影响代码：
  - `src/simd/fastscan.cpp`
  - `include/vdb/simd/fastscan.h`（预计不需要改 public signature）
  - `tests/simd/prepare_query_test.cpp` 或相关 FastScan SIMD 测试
  - 可能补充 benchmark/perf 记录文档
- 不改变：
  - 索引格式
  - packed code / packed LUT layout
  - `simd::AccumulateBlock` public API
  - Stage1 / Stage2 / rerank 语义
  - benchmark JSON 字段名
- 风险：
  - pointer increment 和 LUT plane layout 必须与现有实现完全一致。
  - chunked unroll 可能带来有限代码体积增长，但应明显低于逐维度模板实例化。
