## 1. Runner 与 Readiness Gate

- [x] 1.1 在 formal-study scripts 下新增或扩展 `run-five-dataset-vecfetch-pareto-nlist6400` 的正式 runner。
- [x] 1.2 按 `design.md` 中列出的路径，编码五个数据集的 accepted VecFetch index map。
- [x] 1.3 在调度运行前验证每个 accepted VecFetch 索引目录，包括目录存在性、必需文件和预期 `nlist`。
- [x] 1.4 增加 no-build guard：如果必需 `--index-dir` 缺失或不匹配，正式 VecFetch 运行必须在 benchmark 执行前失败。
- [x] 1.5 验证每个数据集都有 `gt_top100.npy`，并确认正式运行不会使用 `gt_top10.npy`。
- [x] 1.6 在启动昂贵运行前检查 CPU 空闲、活跃 benchmark 进程和剩余磁盘空间。

## 2. Dataset Adapter Gate

- [x] 2.1 验证 Amazon ESCI、ImageNet1K split-v1 和 VoxCeleb2 split-v1 的 `bench_e2e` adapter 支持 1000-query 正式运行。
- [x] 2.2 验证或创建 COCO100K 的正式 `bench_e2e` adapter 表面，且不重建 accepted index。
- [x] 2.3 验证或创建 MS MARCO 的正式 1000-query `bench_e2e` adapter 表面，且不重建 accepted index。
- [x] 2.4 增加 adapter 检查：query count、query id 对齐、base id policy、payload source 可用性和 `gt_top100.npy` 行数。
- [x] 2.5 将 adapter readiness 结果记录到本 change 的输出目录。

## 3. Full VecFetch 命令矩阵

- [x] 3.1 按 `design.md` 中的数据集专属 `nprobe` 网格生成 `topk={10,100}` 的 Full VecFetch 命令。
- [x] 3.2 确保每条 Full VecFetch 命令都显式传入 `--index-dir`、`--gt-file .../gt_top100.npy`、`--dynamic-safeout 1` 和 `--dynamic-safein frontier`。
- [x] 3.3 确保每条 Full VecFetch 命令使用 `dynamic_safein_stable_probes=1`、`dynamic_safein_rel_tol=0.005`、`dynamic_safein_defer_initial_clusters=4` 和 `dynamic_safein_defer_until_ready=1`。
- [x] 3.4 确保每条 Full VecFetch 命令不传已删除的 cluster-loading CLI，并使用 `--io-queue-depth 64`、`--fixed-vec-buffer-count 1024` 和 `--bits 4`；输出必须记录 resident full-preload metadata。
- [x] 3.5 dry-run 完整 Full VecFetch 命令矩阵，并验证没有任何命令会构建或覆盖 accepted index。

## 4. Full VecFetch Smoke 与 Sweep 执行

- [x] 4.1 对每个数据集运行 `topk=10` 和 `topk=100` smoke 点；当 `nprobe=64` 属于该数据集网格时使用 `nprobe=64`。
- [x] 4.2 验证 smoke 输出报告 `recall_available=true`、使用 `gt_top100.npy`，并且 `index_source` 符合复用索引运行口径。
- [x] 4.3 先运行 COCO100K 和 VoxCeleb2 的 Full VecFetch sweep。
- [x] 4.4 小数据集 sweep 验证通过后，运行 Amazon ESCI 和 ImageNet1K 的 Full VecFetch sweep。
- [x] 4.5 最后运行 MS MARCO 的 Full VecFetch sweep。
- [x] 4.6 如果磁盘空间不安全、CPU/进程检查失败，或某个必需运行使用了错误 GT/index source，立即停止并报告。

## 5. ImageNet1K IVF nlist 6400 Baseline 重跑

- [x] 5.1 更新或扩展 baseline runner，使 ImageNet1K IVF 重跑可以使用 `nlist=6400`，且不改变其它数据集。
- [x] 5.2 生成 ImageNet1K `nlist=6400` IVF+PQ 命令，覆盖 FlatStor 和 Lance、`topk={10,100}`、`nprobe={16,32,64,128,256,512}`。
- [x] 5.3 生成 ImageNet1K `nlist=6400` IVF+RQ vector-search 命令，并在 FlatStor 与 Lance coupled E2E 行之间复用每个 vector output。
- [x] 5.4 确保没有任何 ImageNet1K IVF 命令调度 `nprobe=96`、`192`、`384`、`768` 或 `1024`。
- [x] 5.5 Full VecFetch smoke 通过后，运行 ImageNet1K `nlist=6400` IVF+PQ 与 IVF+RQ sweep。
- [x] 5.6 保留旧 ImageNet1K `nlist=4096` IVF 输出，不删除、不覆盖。

## 6. 聚合与 Pareto 输出

- [x] 6.1 创建专用输出根目录 `/home/zcq/VDB/baselines/formal-study/outputs/vecfetch_pareto_nlist6400/`。
- [x] 6.2 将 Full VecFetch `results.json` 解析成 Pareto-ready CSV，包含索引路径、top-k、nprobe、recall、latency 和机制字段。
- [x] 6.3 解析 ImageNet1K `nlist=6400` IVF baseline 重跑输出，并从本 change 对比中排除 ImageNet1K `nlist=4096` 行。
- [x] 6.4 将 Full VecFetch 行与五个数据集的六种 accepted baseline 组合合并。
- [x] 6.5 DiskANN 行只从 accepted raw-vector baseline 结果面读取，不重建、不替换 DiskANN 行。
- [x] 6.6 为 `Recall@10` 目标 `0.90,0.95,0.98` 和 `Recall@100` 目标 `0.80,0.90,0.95,0.995` 生成 threshold-selected summaries。
- [x] 6.7 对未达到的 recall target 标记 `best-effort`，不得静默省略。
- [x] 6.8 基于验证后的 combined Pareto CSV 生成五个数据集的 Pareto 图表，每个数据集分别生成 `Recall@10` 和 `Recall@100` 图，纵轴使用 QPS（`1000 / avg_ms`）。
- [x] 6.9 将 Pareto 图表写入 `/home/zcq/VDB/baselines/formal-study/outputs/vecfetch_pareto_nlist6400/plots/`，并确保图表输入排除 `topk=20/50`、ImageNet1K IVF `nlist=4096` 和 `gt_top10.npy` VecFetch 行；Full VecFetch 曲线保留全部有效 `nprobe` 点，包括被 QPS/recall Pareto 判定为 dominated 的点；DiskANN 曲线使用全部 raw-vector 参数组合，先取 QPS Pareto frontier，当 frontier 超过 8 个点时，在相近 recall 区间内保留 QPS 最高的 5-8 个代表点。

## 7. 验证与报告

- [x] 7.1 验证正式输出只包含 `topk=10` 和 `topk=100`。
- [x] 7.2 验证每个正式 Full VecFetch 行都引用 accepted index path 和 `gt_top100.npy`。
- [x] 7.3 验证本 change 的 ImageNet1K IVF 行只使用 `nlist=6400` 和六个必需 `nprobe` 值。
- [x] 7.4 验证每个 `dataset x system x topk` 组至少有一个有效 Pareto 点，或有显式 failure record。
- [x] 7.5 验证每个数据集都有 `Recall@10` 和 `Recall@100` Pareto 图表，或报告中有显式缺口记录。
- [x] 7.6 验证报告列出所有可复用索引路径、被排除的历史行、输出路径、Pareto 图表路径和 best-effort threshold gaps。
- [x] 7.7 运行结束后记录最终磁盘空间和活跃 benchmark 进程状态。
- [x] 7.8 运行 OpenSpec status/apply-instruction 验证，确认所有任务可被 implementation tracking 解析。
