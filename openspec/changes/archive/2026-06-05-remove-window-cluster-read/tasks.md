## 1. Scope Audit

- [x] 1.1 使用 code-review-graph 和源码确认 `SearchConfig`、`OverlapScheduler`、`bench_e2e`、`bench_vector_search`、benchmark scripts、`test_overlap_scheduler` 中所有 `clu_read_mode`、`use_resident_clusters`、`prefetch_depth`、`refill_threshold`、`refill_count`、`CLUSTER_BLOCK` 引用。
- [x] 1.2 记录当前 COCO100k test_config 或历史主锚点参数，包括 index path、query count、`nlist`、`nprobe`、`topk`、`bits`、epsilon/SafeIn/SafeOut 设置。
- [x] 1.3 记录当前 MS MARCO test_config 或历史主锚点参数，包括 adapter/dataset path、index path、GT path、query count、`nlist`、`nprobe`、`topk`、`bits` 和 rotator/build 口径。

## 2. Query Config Cleanup

- [x] 2.1 从 `SearchConfig` 中删除或正式废弃 `CluReadMode`、`clu_read_mode`、`use_resident_clusters`、`prefetch_depth`、`refill_threshold`、`refill_count`。
- [x] 2.2 更新 `SearchConfig` 默认语义，使查询默认要求 resident full-preload cluster views。
- [x] 2.3 清理 `CluReadModeName` 或等价格式化函数，并删除所有 active config/output 对这些字段的依赖。
- [x] 2.4 检查 `SearchStats` 中只服务 cluster window read 的字段，例如 query-time cluster parse、prefetch submit、prefetch wait；删除字段或确认字段已被其他正式语义复用。

## 3. OverlapScheduler Resident-Only Path

- [x] 3.1 修改 `OverlapScheduler::Search`，移除 window/full_preload 分支选择，固定进入 resident cluster probe path。
- [x] 3.2 在搜索开始前确认 resident cluster views 可用；若不可用，则执行明确 lazy preload 或返回清晰错误，不允许回退到 window cluster read。
- [x] 3.3 将 `ProbeResidentThinPath` 重命名或收敛为正式主路径函数，例如 `ProbeResidentClusters` 或 `ProbePreloadedClusters`。
- [x] 3.4 删除 `PrefetchClusters`、`SubmitClusterRead` 以及 `ProbeAndDrainInterleaved` 中等待 cluster block、维护 `ready_clusters_`、`inflight_clusters_`、`next_to_submit_`、refill 的逻辑。
- [x] 3.5 从 `PendingIO::Type` 删除 `CLUSTER_BLOCK`，并删除 `PendingIO` 中仅服务 cluster block 的字段或注释。
- [x] 3.6 删除 `DispatchCompletion()` 中 query-time cluster block parse 分支，保留并验证 `VEC_ONLY`、`VEC_ALL`、`PAYLOAD` completion 行为。
- [x] 3.7 清理 `OverlapScheduler` 成员变量、注释和 pipeline 文档，确保不再描述 sliding-window cluster prefetch。
- [x] 3.8 确认 SafeIn/SafeOut/Uncertain、dynamic SafeOut frontier、vector read batching、payload fetch、rerank 和 final drain 语义未改变。

## 4. Benchmark And Script Cleanup

- [x] 4.1 从 `bench_e2e` 删除或拒绝 `--clu-read-mode`、`--use-resident-clusters`、`--prefetch-depth`、`--refill-threshold`、`--refill-count`。
- [x] 4.2 修改 `bench_e2e`，在 measured query batch 前固定执行或确认 `.clu` full preload，并继续输出 preload time、resident memory/bytes 和 query hot path breakdown。
- [x] 4.3 从 `bench_e2e` JSON/CSV/config 输出中删除 active window/prefetch/refill 字段；若短期保留兼容字段，必须标记为 deprecated/no-op 且不得影响查询行为。
- [x] 4.4 从 `bench_vector_search` 删除 prefetch/refill 配置赋值，并确保搜索前 resident cluster views 可用。
- [x] 4.5 清理 `benchmarks/scripts/run_hotpath_experiments.py`、`hotpath_experiments.md` 和 MS MARCO 相关 runner 中的 `window-read`、`no-resident`、prefetch/refill 变体。
- [x] 4.6 更新 benchmark 帮助文本和文档，明确正式 cluster-side query data path 只有 resident full preload。

## 5. Tests

- [x] 5.1 删除或改写 `test_overlap_scheduler` 中只验证 `prefetch_depth`、`refill_threshold`、`refill_count` 或 window/full_preload 对比的测试。
- [x] 5.2 新增或更新 resident-only 搜索测试，验证 fixed-`nprobe` 下结果排序、candidate 统计和 no-`CLUSTER_BLOCK` 语义。
- [x] 5.3 新增未 preload 场景测试：scheduler 必须明确 preload 或明确失败，不得进入 window fallback。
- [x] 5.4 更新 completion/buffer lifecycle 测试，确认删除 `CLUSTER_BLOCK` 后 `VEC_ONLY`、`VEC_ALL`、`PAYLOAD` cleanup 没有泄漏或重复释放。
- [x] 5.5 跑受影响回归测试：`test_overlap_scheduler`、`test_io_uring_reader`、`test_pread_fallback_reader`、`test_buffer_pool`、`test_rerank_consumer`、`test_ivf_index`、`test_cluster_store`。

## 6. Build And Static Validation

- [x] 6.1 编译核心库、测试和 benchmark targets，确认删除配置字段后无残留编译引用。
- [x] 6.2 使用 `rg` 确认正式代码和脚本中不再存在 active window cluster read 控制路径；允许只在 archive/OpenSpec 历史文档中出现旧字符串。
- [x] 6.3 运行 `openspec validate remove-window-cluster-read --strict` 或等价校验，确认 proposal/design/specs/tasks 格式通过。

## 7. COCO100k Validation

- [x] 7.1 使用 COCO100k 当前 test_config 或历史主锚点 `nlist=2048,nprobe=64,topk=10,bits=4,epsilon=0.90` 运行 `bench_e2e`。
- [x] 7.2 若此前同参包含 vector-only 验证，则使用同一 COCO100k 参数运行 `bench_vector_search`。
- [x] 7.3 记录完整命令、index path、query count、GT 来源、是否计入 preload、preload time、resident memory/bytes。
- [x] 7.4 报告 COCO100k 的 recall、avg/p50/p95/p99 latency、Stage1 SafeIn/SafeOut/Uncertain、Stage2 SafeIn/SafeOut/Uncertain、false SafeIn/false SafeOut 可用字段。
- [x] 7.5 与删除前同参结果对比；如参数或索引不一致，明确标注不可直接归因的差异。

## 8. MS MARCO Validation

- [x] 8.1 使用 MS MARCO 当前 test_config 或历史主锚点 `nlist=16384,nprobe=256,topk=10,bits=4` 运行 `bench_e2e`。
- [x] 8.2 优先复用现有 MS MARCO adapter、GT 和已构建索引；如需要重建或重新生成 adapter，记录原因和命令。
- [x] 8.3 记录完整命令、adapter/dataset path、index path、GT path、query count、rotator/build 口径、是否计入 preload、preload time、resident memory/bytes。
- [x] 8.4 报告 MS MARCO 的 recall、avg/p50/p95/p99 latency、Stage1 SafeIn/SafeOut/Uncertain、Stage2 SafeIn/SafeOut/Uncertain、false SafeIn/false SafeOut 可用字段。
- [x] 8.5 与删除前同参结果对比；如参数或索引不一致，明确标注不可直接归因的差异。

## 9. Reporting

- [x] 9.1 在 change 目录下新增 validation 记录，保存 COCO100k 和 MS MARCO 的命令、环境、结果表和关键日志路径。
- [x] 9.2 总结删除 window cluster read 后的代码路径变化、速度变化、SafeIn/SafeOut/Uncertain 变化和剩余风险。
- [x] 9.3 如果 COCO100k 或 MS MARCO 出现 recall 或 latency 回退，定位是 preload 口径、resident probe、raw vector I/O、SafeOut frontier 还是 benchmark 参数差异导致。
