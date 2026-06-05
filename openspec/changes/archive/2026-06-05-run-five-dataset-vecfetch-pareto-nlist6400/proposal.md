## Why

更新后的 VecFetch 方法在初步测试中已经表现出明显速度提升，但当前正式结果面还没有在五个数据集上按 baseline 表相同的 `Recall@10` / `Recall@100` 口径形成 Full VecFetch Pareto sweep。与此同时，ImageNet1K 现在采用 `nlist=6400` 的可复用 VecFetch 索引，因此 IVF baseline 也必须在相同 `nlist` 下重跑，保证比较公平。

## What Changes

- 为 `coco_100k`、`msmarco_passage`、`amazon_esci`、`imagenet1k` 和 `voxceleb2_ecapa_150k` 补跑五数据集 Full VecFetch Pareto 实验。
- 所有 VecFetch 实验只复用已有索引；如果必需索引缺失，runner 必须在构建前失败。
- 在实验文档和运行输出中记录每个数据集使用的可复用 VecFetch 索引路径。
- 正式 `topk=10` 与 `topk=100` 运行都使用 `gt_top100.npy` 作为 ground truth 来源。
- Full VecFetch 主要 sweep `nprobe`，其它方法配置固定：启用 Dynamic SafeOut、Dynamic SafeIn `frontier`、deferred frontier readiness、resident/full-preload 查询模式，以及已有 combined/raw-payload 索引布局。
- 在 `nlist=6400` 下重跑 ImageNet1K 的 IVF+PQ 和 IVF+RQ baseline，并分别覆盖 FlatStor 与 Lance 后端。
- ImageNet1K `nlist=6400` IVF baseline 只跑 `nprobe={16,32,64,128,256,512}` 六个点。
- 聚合 Pareto-ready 输出，使 ImageNet1K IVF 对比只使用新的 `nlist=6400` baseline 结果；旧 `nlist=4096` ImageNet1K IVF 行保留在磁盘上，但从本次对比中排除。
- 保留已经接受的 raw-vector DiskANN baseline 结果面；本 change 不重建 DiskANN 索引。
- 在 formal-study 输出树下生成结果摘要和 Pareto 输入，并对未达到的 recall 目标显式标记 `best-effort`。
- 在正式实验结束并完成 combined Pareto CSV 验证后，绘制五个数据集的 `Recall@10` / `Recall@100` Pareto 曲线图表，供后续查看和论文图表筛选。

## Capabilities

### New Capabilities

无。

### Modified Capabilities

- `formal-baseline-execution`：增加五数据集 Full VecFetch Pareto 执行面，以及 ImageNet1K IVF `nlist=6400` 重跑要求。
- `formal-baseline-result-tracking`：增加复用索引身份、ImageNet1K `nlist=6400` 过滤、top-k/GT 一致性、Pareto-ready 聚合与五数据集 Pareto 图表生成的结果追踪要求。
- `e2e-benchmark`：要求正式 VecFetch E2E 运行复用已有索引，从 top-100 GT 导出 `Recall@10` / `Recall@100`，并保留 Pareto 与机制归因所需字段。

## Impact

- 受影响的实验脚本预计包括 ICDE baseline runner、新增或扩展的 VecFetch Pareto runner、聚合/报告工具，以及让五个数据集都能通过 `bench_e2e` 运行所需的小型 adapter 检查。
- 受影响输出位于 `/home/zcq/VDB/baselines/formal-study/outputs/`，本 change 使用专门结果面，避免在验证前覆盖既有 baseline summary。
- 默认不得删除或重建任何已有 VecFetch、IVF 或 DiskANN 索引。
- 不计划修改应用 API 或索引格式。
