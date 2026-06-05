## Context

当前查询代码同时保留了两类 cluster-side 数据访问路径：

- window cluster read：按 `prefetch_depth` 提交 `CLUSTER_BLOCK` I/O，按 `refill_threshold/refill_count` 补读，并用 `ready_clusters_` 等待目标 cluster。
- full preload resident：查询前把 `.clu` 全量预加载，在线阶段直接使用 resident parsed cluster view。

论文后续只讨论 full preload resident 场景，window read 不再是实验变量。现有默认和脚本中仍存在 `--clu-read-mode`、`--use-resident-clusters`、prefetch/refill 参数和 window/full_preload 对比矩阵，这会让主搜索路径继续背负不使用的分支。

## Goals / Non-Goals

**Goals:**

- 删除查询时 window cluster block read、prefetch/refill 和 `CLUSTER_BLOCK` completion 路径。
- 将 resident full preload 设为唯一正式 cluster probe 方式。
- 简化 `SearchConfig`、`OverlapScheduler`、benchmark CLI、脚本和测试矩阵。
- 保留 SafeIn/SafeOut/Uncertain、fixed-`nprobe`、vector read、payload fetch、rerank 和 final drain 结果语义。
- 不修改 `.clu` 存储格式，允许复用已有 COCO/MS MARCO 索引。
- 实现后用相同参数重测 COCO100k 和 MS MARCO，报告速度、recall、SafeIn/SafeOut/Uncertain 与 preload 指标。

**Non-Goals:**

- 不引入新的 cluster 文件格式或索引重建要求。
- 不改变 RaBitQ 分类公式、SafeIn/SafeOut frontier 或 top-k 语义。
- 不重新引入 probing early-stop。
- 不优化 raw vector/payload I/O 的 batching 细节；本 change 只删除 cluster-side window read 分支。
- 不保留 window/full_preload 模式对比作为正式 benchmark 目标。

## Decisions

### Decision 1: 硬删除 window 模式，而不是保留 no-op 参数

正式查询配置将不再支持 `window` cluster loading。`SearchConfig` 中的 `clu_read_mode`、`use_resident_clusters`、`prefetch_depth`、`refill_threshold`、`refill_count` 应删除或降级为内部兼容 no-op，并且 benchmark CLI 不再暴露这些参数。

理由：

- 后续论文和实验不再使用 window 作为对照。
- 保留 no-op 参数会继续误导结果解释，例如用户以为调 prefetch/refill 会影响性能。
- 删除配置可以让编译器和测试直接暴露仍依赖旧分支的代码。

备选方案是保留参数但强制 `full_preload`。该方案迁移成本低，但会让 benchmark 输出和脚本继续携带无效字段，本轮不采用。

### Decision 2: `OverlapScheduler::Search` 只进入 resident probe path

目标查询流程为：

```text
Search()
  -> 确保 resident cluster views 已可用
  -> FindNearestClusters(query, nprobe)
  -> ProbeResidentClusters(sorted_clusters)
  -> Flush/drain vector and payload requests
  -> Finalize top-k
```

需要删除：

- `PrefetchClusters`
- `SubmitClusterRead`
- `ProbeAndDrainInterleaved` 中等待 cluster block、refill 和 `ready_clusters_` 的逻辑
- `PendingIO::Type::CLUSTER_BLOCK`
- `DispatchCompletion()` 的 cluster block parse 分支
- `ready_clusters_`、`next_to_submit_`、`inflight_clusters_`

需要保留：

- resident thin path 的 probe/classify/submit 逻辑
- vector-only、all-read、payload completion 路径
- SafeOut frontier、SafeIn/Uncertain 统计和 final drain

`ProbeResidentThinPath` 可以重命名为 `ProbeResidentClusters` 或 `ProbePreloadedClusters`，表示它不再是可选 thin path，而是主路径。

### Decision 3: preload 责任前移到 benchmark warm-up，scheduler 保留安全检查

`bench_e2e` 和 `bench_vector_search` 应在搜索前明确执行或确认 `.clu` full preload。`OverlapScheduler` 内部应保留轻量安全检查：如果 resident cluster views 不存在，则执行一次 lazy preload 或返回明确错误。建议优先采用“benchmark 显式 preload + scheduler 防御检查”的组合。

理由：

- benchmark 需要把 preload 成本与 query hot path 成本分开记录。
- scheduler 需要避免被单测或临时工具以未预热索引调用时产生空指针或错误结果。
- 不改变旧索引格式，迁移成本只在运行配置层。

### Decision 4: benchmark 输出删除 window/prefetch 字段，保留 preload 和 query breakdown

`bench_e2e`/`bench_vector_search` 不再接受或记录：

- `clu_read_mode`
- `use_resident_clusters`
- `prefetch_depth`
- `refill_threshold`
- `refill_count`

继续记录：

- `preload_time_ms`
- `preload_bytes`
- resident cluster memory footprint
- `coarse_select_ms`
- `probe_prepare_ms`
- `probe_stage1_ms`
- `probe_stage2_ms`
- `probe_classify_ms`
- `probe_submit_ms`
- SafeIn/SafeOut/Uncertain 统计
- recall 和 latency

`parse_cluster_ms`、`prefetch_submit_ms`、`prefetch_wait_ms` 若只服务 cluster window read，应删除或标记为不再输出；如果字段名已被其他含义复用，需要在实现时逐项确认后再清理。

### Decision 5: 验证以同参重测为准

实现结束后必须复测两类数据集：

- COCO100k：复用当前 COCO test_config 或历史主锚点 `nlist=2048,nprobe=64,topk=10,bits=4,epsilon=0.90`，索引可复用 `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90`。
- MS MARCO：复用当前 MS MARCO test_config 或历史主锚点 `nlist=16384,nprobe=256,topk=10,bits=4`，优先使用现有 adapter/GT 和已构建索引。

报告中必须记录完整命令、索引路径、query 数、GT 来源、是否计入 preload，以及主要结果字段。若 test_config 与历史主锚点不一致，以 test_config 为准，并在结果中说明差异。

## Risks / Trade-offs

- [Risk] 删除 window 模式后无法在低内存环境运行查询。  
  Mitigation: 将 full preload 内存占用作为正式前提并在 benchmark 输出中报告；如果未来需要低内存模式，另开 change 设计。

- [Risk] 单测或工具过去依赖默认 `clu_read_mode=window`。  
  Mitigation: 修改默认构造和测试 fixture，使搜索前显式 preload；新增未 preload 时的明确错误或 lazy preload 测试。

- [Risk] 删除 `CLUSTER_BLOCK` 后误伤 vector/payload I/O completion。  
  Mitigation: 只删除 cluster block request type；保留 `VEC_ONLY`、`VEC_ALL`、`PAYLOAD` 的 pending slot、buffer ownership 和 final drain 测试。

- [Risk] benchmark 结果因是否计入 preload 发生口径漂移。  
  Mitigation: query latency 和 preload latency 分开记录；COCO/MS MARCO 重测结果中同时报告 preload 成本和 query-only/hot path 指标。

- [Risk] OpenSpec 中仍有旧 spec 提到 window/full_preload 对比。  
  Mitigation: 本 change 同步修改 `async-cluster-prefetch`、`clu-full-preload`、`resident-thin-query-path`、`resident-query-hotpath`、`query-pipeline`、`e2e-benchmark` 和 `benchmark-infra` 的需求。

## Migration Plan

1. 删除或废弃配置入口，先让编译暴露仍引用 prefetch/refill/window 的位置。
2. 收敛 `OverlapScheduler` 到 resident-only probe path，删除 cluster block completion。
3. 更新 `bench_e2e`、`bench_vector_search` 和 benchmark scripts，固定执行 full preload。
4. 改写或删除 window/prefetch/refill 测试，保留 resident-only 正确性和统计测试。
5. 运行单元测试和 COCO/MS MARCO 同参 benchmark。

## Open Questions

- None. 本 change 直接采用 hard-delete 策略；不保留 window 模式作为 deprecated 运行入口。
