## Why

近期优化实验已经进入技术冻结阶段，部分实验性查询功能在同口径测试中表现为负收益或收益不稳定，但仍暴露在配置、CLI、统计和热路径分支中。这些冗余功能增加了维护成本、测试矩阵和论文实验口径的不确定性，需要在冻结前收敛到稳定的 resident 查询主路径。

## What Changes

- 删除或隐藏负收益查询功能的用户可见入口和主路径分支：
  - Stage2 progressive active-bits / per-bit pruning。
  - Stage1 block-level skip envelope。
  - vector read address sorting。
  - budgeted early submit。
- 保守处理 SIMD：
  - 本轮不删除底层 SIMD helper/kernel，避免误伤仍被固定路径、兼容路径或测试复用的实现。
  - 仅移除上层已放弃功能对这些 SIMD helper 的调用入口、CLI 和统计口径。
- 收敛 resident 查询配置：
  - 默认保留 fixed active ex_bits 查询。
  - 保留 selective preload、compact batched preload、two-level coarse routing、SafeIn/SafeOut pipeline。
  - 保留 `vector_bitplanes` official-like baseline/兼容路径和当前冻结的 `tile_lane_bitmajor` 方法格式。
- 清理 benchmark 输出：
  - 移除废弃功能对应的 CLI 参数、JSON 字段和派生统计。
  - 保留正式对比需要的 recall、QPS/latency、probe breakdown、stage1/stage2 主路径统计。
- 增加删除前后验证要求：
  - 在同一已有索引、同一 warm query 设置下对比删除前最佳结果与删除后结果。
  - 验证 recall 和速度没有超出实验误差范围的损失。

## Capabilities

### New Capabilities

- None.

### Modified Capabilities

- `query-pipeline`: 收敛查询管线的正式支持范围，移除负收益实验分支的主路径可达性。
- `resident-query-hotpath`: 明确 resident hot path 的冻结配置和必须保留的稳定优化。
- `exrabitq-storage-layout`: 明确冻结期保留的 stage2 布局和 SIMD 保守边界，避免误删兼容/对照格式。
- `e2e-benchmark`: 增加删除前后同口径回归验证要求，并移除废弃统计字段的正式输出要求。

## Impact

- Affected code:
  - Query configuration and stats: `include/vdb/query/search_context.h`
  - Query scheduling: `include/vdb/query/overlap_scheduler.h`, `src/query/overlap_scheduler.cpp`
  - Cluster probing: `include/vdb/index/cluster_prober.h`, `src/index/cluster_prober.cpp`
  - Resident cluster parsing/preload metadata for removed Stage1 envelope: `include/vdb/query/parsed_cluster.h`, `include/vdb/storage/cluster_store.h`, `src/storage/cluster_store.cpp`
  - Benchmarks: `benchmarks/bench_online_query.cpp`, `benchmarks/bench_e2e.cpp`
  - Tests covering removed flags/branches.
- Compatibility:
  - Existing frozen-format indexes should remain readable.
  - Current official-like baseline layout should remain available.
  - Experimental CLI flags and JSON fields for removed features may be breaking for old experiment scripts.
- Validation:
  - Build and unit tests must pass after cleanup.
  - Warm query benchmark must be rerun on representative frozen settings and compared against the best pre-cleanup result.
