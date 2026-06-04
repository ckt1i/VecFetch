## ADDED Requirements

### Requirement: VecFetch Pareto 结果行必须保留索引身份和正式 recall 来源
本 change 生成的每个 Full VecFetch 结果行都 MUST 记录足够 metadata，用于证明该行由哪个可复用索引、哪个 ground truth 文件和哪个正式 query protocol 生成。

#### Scenario: Full VecFetch 行包含可复用索引 metadata
- **WHEN** 一个 Full VecFetch 运行完成
- **THEN** 输出行必须包含 `dataset`、`system=Full VecFetch`、`topk`、`nprobe`、`index_dir`、`index_id`、`nlist`、`fixed_vec_buffer_count`、`gt_file` 和 `query_count`
- **AND** `index_dir` 必须等于该数据集的 accepted index path
- **AND** 正式行的 `query_count` 必须为 `1000`。

#### Scenario: recall 字段来自 top-100 GT
- **WHEN** 聚合一个 Full VecFetch 行
- **THEN** 该行必须表明 recall 来自 `gt_top100.npy`
- **AND** 对 `topk=10`，`recall_at_topk` 必须被解释为 `Recall@10`
- **AND** 对 `topk=100`，`recall_at_topk` 必须被解释为 `Recall@100`
- **AND** 由 `gt_top10.npy` 生成的行不得进入本 change 的正式 Pareto 结果面。

### Requirement: 聚合必须将新 Pareto 结果面与历史输出隔离
结果追踪工作流 MUST 将本 change 的输出写入专用位置，并保留历史行但按新过滤规则排除不合格行。

#### Scenario: 使用专用输出根目录
- **WHEN** 本 change 聚合运行结果
- **THEN** 必须将输出写入 `/home/zcq/VDB/baselines/formal-study/outputs/vecfetch_pareto_nlist6400/`
- **AND** 验证通过前不得覆盖此前的 `icde_baseline_summary.csv` 或 `icde_baseline_selected.csv`。

#### Scenario: 排除历史 ImageNet1K nlist 4096 IVF 行
- **WHEN** 为本 change 选择 ImageNet1K IVF 对比行
- **THEN** 只有 `nlist=6400` 的行可以进入对比
- **AND** 历史 ImageNet1K IVF `nlist=4096` 行必须保留在磁盘上，但必须从新的 Pareto 和 threshold-selection 输出中排除
- **AND** 如果发现被排除的历史行，报告必须记录排除数量。

#### Scenario: DiskANN 行保持为 accepted baseline reference
- **WHEN** baseline 行与 Full VecFetch 行合并
- **THEN** DiskANN 行必须来自 accepted raw-vector baseline 结果面
- **AND** 本 change 不得重建或替换 DiskANN 行
- **AND** DiskANN 行必须保留既有 accepted `index_id` 值。

### Requirement: Pareto 与 threshold 输出必须覆盖 Recall@10 和 Recall@100
新 summary MUST 支持两个正式 top-k 档位的论文级 Pareto 作图和阈值选择。

#### Scenario: Pareto CSV 包含全部对比系统
- **WHEN** 聚合完成
- **THEN** combined Pareto CSV 必须包含 Full VecFetch 和六种 baseline 组合：
  - `IVF+PQ+FlatStor`
  - `IVF+PQ+Lance`
  - `IVF+RQ+FlatStor`
  - `IVF+RQ+Lance`
  - `DiskANN+FlatStor`
  - `DiskANN+Lance`
- **AND** 每一行必须包含 recall、average latency、p50、p95、p99、可用的 bytes/read counters，以及足以重建绘图点的参数 metadata。

#### Scenario: threshold selection 保留 best-effort 标记
- **WHEN** 生成 threshold-selected summaries
- **THEN** `Recall@10` targets 必须包括 `0.90`、`0.95` 和 `0.98`
- **AND** `Recall@100` targets 必须包括 `0.80`、`0.90`、`0.95` 和 `0.995`
- **AND** 如果某个系统在某个数据集上无法达到目标，selected row 必须标记为 `best-effort`，不得省略或视为运行失败。

#### Scenario: top-k 范围排除陈旧行
- **WHEN** 本 change 写出 Pareto 或 selected 输出
- **THEN** 输出只能包含 `topk=10` 和 `topk=100`
- **AND** `topk=20` 或 `topk=50` 的行必须从本 change 的正式 summary 中排除。

### Requirement: 实验完成后必须绘制五数据集 Pareto 曲线图表
本 change 在正式运行和聚合完成后，MUST 基于已有聚合结果绘制五个数据集下的 Pareto 曲线图表，供后续人工查看和论文图表筛选。

#### Scenario: 每个数据集生成 Recall@10 和 Recall@100 Pareto 图
- **WHEN** combined Pareto CSV 生成并通过验证
- **THEN** 绘图流程必须为 `coco_100k`、`msmarco_passage`、`amazon_esci`、`imagenet1k` 和 `voxceleb2_ecapa_150k` 分别生成 `Recall@10` 与 `Recall@100` Pareto 图
- **AND** 每张图必须以 recall 为横轴、QPS（`1000 / avg_ms`）为纵轴
- **AND** 每张图必须同时展示 Full VecFetch 和六种 baseline 组合。

#### Scenario: 图表输出位置和输入来源可审计
- **WHEN** Pareto 图表生成
- **THEN** 图表必须写入 `/home/zcq/VDB/baselines/formal-study/outputs/vecfetch_pareto_nlist6400/plots/`
- **AND** 绘图脚本或图表 metadata 必须记录使用的 combined Pareto CSV 路径
- **AND** 报告必须列出每个生成图表的路径。

#### Scenario: 图表不得使用未验证或历史错误口径行
- **WHEN** 生成 Pareto 图表
- **THEN** 图表输入必须排除 `topk=20`、`topk=50`、ImageNet1K IVF `nlist=4096` 行和 `gt_top10.npy` VecFetch 行
- **AND** Full VecFetch 图表输入必须保留全部有效 `nprobe` 点，包括在 QPS/recall Pareto 意义下被 dominated 的点
- **AND** DiskANN 图表输入必须使用该后端的全部 accepted raw-vector 参数组合，先取 QPS Pareto frontier
- **AND** 如果某条 DiskANN frontier 超过 8 个点，绘图必须在相近 recall 区间内保留 QPS 最高的代表点，并将曲线压缩到 5-8 个点；如果严格 frontier 少于 5 个点，则保留全部 frontier 点而不补入被支配点
- **AND** 如果某个数据集或系统缺少可绘制点，报告必须记录缺口，而不是静默生成误导性图表。

### Requirement: 报告必须记录执行安全性、可复用索引和验证结果
本 change 的最终报告 MUST 能在无需人工检查日志的情况下审计实验。

#### Scenario: 报告列出所有可复用索引路径
- **WHEN** 生成报告
- **THEN** 报告必须列出五个数据集各自的 accepted VecFetch index path
- **AND** 必须列出 ImageNet1K `nlist=6400` IVF baseline policy
- **AND** 必须声明本 change 没有重建 VecFetch 或 DiskANN 索引。

#### Scenario: 报告记录验证和资源状态
- **WHEN** 所有计划运行和聚合完成
- **THEN** 报告必须记录最终磁盘空间、活跃 benchmark 进程检查、输出路径、完成组数量、被排除的历史行，以及所有 best-effort recall gaps。
