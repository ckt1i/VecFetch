## Why

论文后续不再考虑查询阶段的 window cluster 读路径，`cluster.clu` 数据将只采用 full preload resident 场景。继续保留 window read、prefetch/refill 参数和对比模式会让主搜索路径、benchmark 口径和后续优化分支变复杂，并掩盖真正需要优化的 resident hot path。

## What Changes

- **BREAKING** 删除 query-time window cluster read 能力：搜索过程中不再提交 per-cluster `CLUSTER_BLOCK` I/O，也不再维护 `ready_clusters_`、`inflight_clusters_` 和 refill 状态。
- **BREAKING** 删除或废弃 `SearchConfig` 中的 `clu_read_mode`、`use_resident_clusters`、`prefetch_depth`、`refill_threshold`、`refill_count` 配置语义；正式查询路径固定要求 cluster 数据已 full preload/resident。
- 将 `OverlapScheduler` 的主 probe 路径收敛到 resident thin path：按 `nprobe` 顺序读取 resident parsed cluster view，保留 SafeIn/SafeOut/Uncertain、vector read、payload fetch、rerank 和 final drain 语义。
- 清理 `bench_e2e`、`bench_vector_search` 和 benchmark 脚本中的 window/full_preload 模式选择参数与输出字段；benchmark 输出继续保留 preload 时间、resident 内存占用和 query hot path 分段统计。
- 更新 OpenSpec 需求，明确 window cluster read 不再是支持模式，full preload resident 是唯一正式 cluster-side query data path。
- 实现完成后，使用相同参数重测 COCO100k 和 MS MARCO 数据集，报告速度、recall、SafeIn/SafeOut/Uncertain 以及 preload 相关指标。

## Capabilities

### New Capabilities

- None.

### Modified Capabilities

- `async-cluster-prefetch`: 废弃 sliding-window cluster block prefetch/refill 模式，移除 `CLUSTER_BLOCK` 查询时 I/O 作为正式能力。
- `clu-full-preload`: 将 full `.clu` preload 从可选模式提升为正式查询路径的必需前提。
- `resident-thin-query-path`: resident thin path 不再是 `full_preload + use_resident_clusters` 下的可选快路径，而是唯一正式 cluster probe 路径。
- `resident-query-hotpath`: 删除对 `clu_read_mode=full_preload` 与 `use_resident_clusters=1` 组合开关的依赖，改为默认 resident hot path 语义。
- `query-pipeline`: 查询管线不得再回退到 window cluster read，仍必须保持 fixed-`nprobe`、SafeIn/SafeOut/Uncertain 和 rerank 结果语义。
- `e2e-benchmark`: benchmark CLI、配置记录和结果输出不再支持 window/full_preload 对比模式，改为固定 full preload resident 口径。
- `benchmark-infra`: 清理脚本、测试矩阵和结果字段中已经废弃的 prefetch/refill/window 选项，并新增 COCO/MS MARCO 同参重测任务。

## Impact

- 查询热路径：`include/vdb/query/search_context.h`、`include/vdb/query/overlap_scheduler.h`、`src/query/overlap_scheduler.cpp`。
- Benchmark：`benchmarks/bench_e2e.cpp`、`benchmarks/bench_vector_search.cpp`、`benchmarks/scripts/run_hotpath_experiments.py`、MS MARCO 相关 benchmark 脚本和测试配置。
- 测试：`tests/query/overlap_scheduler_test.cpp` 中依赖 prefetch/refill/window/full_preload 对比的测试需要删除、改写或替换为 resident-only 语义测试。
- 输出兼容：正式输出不再报告 window 模式和 prefetch/refill 配置；preload 成本、resident 内存和 query 分段统计继续保留，方便区分 warm-up 成本与 query hot path 成本。
- 存储兼容：不改变 `.clu` 文件格式和旧索引格式；删除的是查询时加载方式与 CLI/config 入口，不要求重建已有索引。
