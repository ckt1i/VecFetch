## Why

当 IVF 的 `nlist` 很大时，精确 coarse select 需要在每个查询上对所有一层 centroid 打分。就当前 MSMARCO 的运行结果来看，这一路径已经是明显的 CPU 开销来源，继续只做 kernel 级优化的边际收益预计会越来越小。

这个 change 引入一个可选的双层 coarse routing 路径，在保留现有 exact coarse 路径作为 fallback 的前提下，减少每个查询需要打分的一层 centroid 数量；同时在选中的 routing budget 内，仍然对一层 child centroid 做真实打分。

## What Changes

- 为 `IvfIndex::FindNearestClusters()` 增加一个可选的双层 coarse routing 模式。
- 当 `nlist` 超过阈值时，基于现有一层 IVF centroids 构建并缓存一个内存中的 super-centroid 层次结构。
- 默认使用 `m = ceil(nlist / 128)` 作为二层 super-cluster 数量。
- 使用 `candidate_budget = 8 * nprobe` 控制 super-cluster 选择后需要实际打分的一层 child centroid 数量。
- 只对被选中 super-cluster 覆盖的一层 child centroid 打分，并按真实 child-centroid score 选出最终 top-`nprobe` 的一层 cluster。
- 保留 exact coarse selection 作为默认/fallback 路径，并在实验性 routing 近似之外保持现有查询语义不变。
- 导出用于对比双层 routing 与 exact coarse selection 在速度和 recall 上差异所需的 benchmark/config 元数据和诊断信息。

## Capabilities

### New Capabilities
- `two-level-coarse-routing`：面向大 `nlist` IVF 搜索的可选层次化 coarse routing 能力，包含 routing 参数、fallback 行为和 benchmark 诊断字段。

### Modified Capabilities
- 无。

## Impact

- 受影响代码：
  - `include/vdb/index/ivf_index.h`
  - `src/index/ivf_index.cpp`
  - `include/vdb/query/search_context.h`
  - `src/query/overlap_scheduler.cpp`
  - `benchmarks/bench_e2e.cpp`
  - 相关 coarse-selection 和 benchmark 测试
- 受影响行为：
  - exact coarse selection 继续可用，并保持为安全默认值。
  - 双层 routing 作为大 `nlist` 实验场景下的可选模式引入。
  - benchmark 输出会增加足够的元数据，用来区分 exact 和双层 coarse routing 的结果。
- 依赖关系：
  - 复用现有 centroid 数据和 packed coarse scoring 基础设施。
  - 首版实现不要求修改磁盘上的 index 格式。
