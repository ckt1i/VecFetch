## Context

此前 `rerun-icde-recall100-baselines` change 已经在刷新后的 `topk=10,100` 口径下完成五个数据集的六种 baseline 组合。现在的缺口在方法侧：Full VecFetch 的初步速度结果很好，但这些运行大多是一次性 probe，使用过 `gt_top10.npy` 或旧 query/adapter 表面，还没有形成正式的五数据集 Pareto 结果面。

可复用 VecFetch 索引已经存在于 `/home/zcq/VDB/test/data`。本 change 必须把这些索引视为查询实验的不可变输入：

| 数据集 | VecFetch 索引路径 | `nlist` | 说明 |
| --- | --- | ---: | --- |
| `coco_100k` | `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90` | 2048 | Appendix/sanity 数据集；需用正式 `gt_top100.npy` 重跑。 |
| `msmarco_passage` | `/home/zcq/VDB/test/data/MSMARCO/fht_kac_rotator` | 16384 | 现有 adapter 需要先通过 1000-query 正式 gate，再进入 full sweep。 |
| `amazon_esci` | `/home/zcq/VDB/test/data/amazon_esci/vecfetch_nlist8192_np64_bits4_raw_payload_fht_kac/amazon_esci_20260603T190113/index` | 8192 | Raw FlatStor payload FHT-Kac 索引；旧 blocked Hadamard 索引保留但不得进入正式运行。 |
| `imagenet1k` | `/home/zcq/VDB/test/data/imagenet1k_split_v1/vecfetch_nlist6400_np64_bits4_raw_payload/index` | 6400 | 这是新的方法索引；IVF baseline 必须使用相同 `nlist`。 |
| `voxceleb2_ecapa_150k` | `/home/zcq/VDB/test/data/voxceleb2_ecapa_150k/vecfetch_nlist2048_np64_bits4_audio_payload_split_v1_fht_kac/voxceleb2_ecapa_150k_split_v1_20260603T190033/index` | 2048 | Audio payload split-v1 FHT-Kac 索引；旧 blocked Hadamard 索引保留但不得进入正式运行。 |

DiskANN baseline 行已经引用被接受的 raw-vector 索引，不属于本次重跑范围。本 change 唯一需要补跑的 baseline 是 ImageNet1K 的 IVF+PQ / IVF+RQ，且 `nlist=6400`。

## Goals / Non-Goals

**Goals:**

- 为五个数据集在 `topk=10` 和 `topk=100` 下生成 Pareto-ready 的 Full VecFetch 结果面。
- 确保每个正式 VecFetch 运行在每个数据集上只复用一个预先存在的索引路径。
- `topk=10` 与 `topk=100` 的正式 recall 来源都使用 `gt_top100.npy`。
- 重跑 ImageNet1K IVF+PQ 和 IVF+RQ baseline 行，配置为 `nlist=6400`、FlatStor/Lance 后端、`nprobe={16,32,64,128,256,512}`。
- 新结果面在验证通过前与既有 baseline 输出保持隔离。
- 旧 ImageNet1K `nlist=4096` baseline 行保留在磁盘上，但从新的 Pareto 对比中排除。

**Non-Goals:**

- 不重建 VecFetch 索引。
- 不重建或重跑 DiskANN 索引。
- 不运行 Parquet payload 后端。
- 不额外加入超过 `16,32,64,128,256,512` 的 ImageNet1K IVF `nprobe` 点。
- 不修改 VecFetch 索引格式、benchmark CLI 合约或算法语义。

## Decisions

### Decision 1: 使用显式 accepted-index map

runner 应在代码或可检查 artifact 中维护 dataset-to-index map，并在调度查询运行前验证每个目录。验证至少应检查索引目录存在、包含预期 metadata 文件，并在 `build_metadata.json` 中报告预期 dataset-level `nlist`。

这比 glob 自动发现更稳妥，因为 `/home/zcq/VDB/test/` 下有多个 COCO 索引和旧 ImageNet 运行。显式 map 可以避免 runner 误选 ablation 索引或历史 `nlist=4096` 索引。

对于 Amazon ESCI 和 VoxCeleb2，旧的 accepted candidate index 记录为 `blocked_hadamard_permuted`，已经不满足 `remove-legacy-search-modes` 后的 formal path；因此本 change 使用新构建的 FHT-Kac 索引作为 accepted index，旧索引只保留在磁盘上作历史记录。

### Decision 2: adapter readiness 是 full sweep 前置 gate

Amazon ESCI、ImageNet1K split-v1 和 VoxCeleb2 split-v1 已经具备正式 `bench_e2e` adapter。COCO 和 MS MARCO 需要 preflight gate，因为它们现有方法索引和 benchmark 数据路径不完全统一：

- COCO 有 `/home/zcq/VDB/data/coco_100k`，正式 payload/GT 在 `/home/zcq/VDB/baselines/data/formal_baselines/coco_100k`。
- MS MARCO 有 `/home/zcq/VDB/test/msmarco_fht_kac_adapter`，但历史 adapter 使用 200 queries；正式运行需要与 `gt_top100.npy` 对齐的 1000 query rows。

apply 阶段应先补齐或验证 adapter 表面，再启动 full sweep。缺失 adapter 应阻塞对应数据集，而不是静默回退到更小 query 集。

### Decision 3: Full VecFetch 使用单点运行，不使用 `--nprobe-sweep`

`bench_e2e` 内置 `--nprobe-sweep`，但现有 sweep CSV 主要面向 `recall@10`，没有暴露 `Recall@100` Pareto 和机制归因所需的完整结果面。runner 应对每个 `dataset x topk x nprobe` 点单独启动 benchmark 进程，并解析 `results.json`。

这样 `recall_at_k` 的含义保持清晰：

- `topk=10` -> `Recall@10 = recall_at_k`
- `topk=100` -> `Recall@100 = recall_at_k`

### Decision 4: 固定 Full VecFetch 方法控制项

正式方法 sweep 只改变 `nprobe`。所有机制控制项固定为已接受的方法配置：

```text
dynamic_safeout=1
dynamic_safein=frontier
dynamic_safein_stable_probes=1
dynamic_safein_rel_tol=0.005
dynamic_safein_defer_initial_clusters=4
dynamic_safein_defer_until_ready=1
io_queue_depth=64
fixed_vec_buffer_count=1024
bits=4
```

`clu_read_mode`、`use_resident_clusters` 和 `prefetch_depth` 不再作为命令行控制项传入；`bench_e2e` 的正式路径已经固定为 resident full-preload，并会在输出中记录 `cluster_loading=resident_full_preload`、preload bytes 和 resident memory。

`fixed_vec_buffer_count=1024` 属于当前优化方向，且相对数据集/索引规模内存成本较小。每个输出行都应记录该设置，便于后续图区分本次正式运行和此前使用隐式默认值的 probe。

### Decision 5: 使用紧凑 nprobe 网格

Full VecFetch nprobe 网格：

| 数据集 | `nprobe` values |
| --- | --- |
| `coco_100k` | `16,32,64,128,256,512` |
| `msmarco_passage` | `16,32,64,128,256,512,1024` |
| `amazon_esci` | `16,32,64,128,256,512` |
| `imagenet1k` | `16,32,64,128,256,512` |
| `voxceleb2_ecapa_150k` | `8,16,32,64,128,256` |

ImageNet1K IVF baseline 重跑：

```text
systems = IVF+PQ, IVF+RQ
backends = flatstor,lance
topk = 10,100
nlist = 6400
nprobe = 16,32,64,128,256,512
candidate_budget = topk * 20
IVF+PQ = m=64, nbits=8
IVF+RQ = total_bits=4
```

ImageNet1K IVF 网格有意排除 `96,192,384,768,1024`，以便和此前 baseline sweep 密度对齐。

### Decision 6: 聚合范围保持隔离且可审计

新聚合应使用专用输出根目录，例如：

```text
/home/zcq/VDB/baselines/formal-study/outputs/vecfetch_pareto_nlist6400/
```

应写出：

- Full VecFetch per-run CSV。
- ImageNet1K `nlist=6400` IVF rerun CSV。
- 合并 Pareto CSV，包含 Full VecFetch 与已接受的六个 baseline 组合。
- 五个数据集的 Pareto 曲线图表，每个数据集分别生成 `Recall@10` 和 `Recall@100` 图，用于后续人工查看和论文图表筛选。
- 报告文件，记录索引复用、被排除的历史行、best-effort recall targets、磁盘状态和活跃进程状态。

只有验证通过后，结果行才应被视为可进入论文图表生成流程。

### Decision 7: 实验结束后基于 verified combined CSV 绘图

Pareto 绘图应在 Full VecFetch sweep、ImageNet1K `nlist=6400` IVF 重跑和 combined Pareto CSV 验证之后执行。绘图输入必须是通过过滤后的 combined CSV，而不是直接扫描历史 `results.json` 或旧 summary。

输出目录固定为：

```text
/home/zcq/VDB/baselines/formal-study/outputs/vecfetch_pareto_nlist6400/plots/
```

每个数据集至少生成两张图：

- `{dataset}_recall10_pareto.png`
- `{dataset}_recall100_pareto.png`

如果绘图工具支持 PDF，也可以同时输出同名 `.pdf`，但 `.png` 是必须产物，便于快速查看。图中横轴为 recall，纵轴为 QPS（`1000 / avg_ms`），图例必须区分 Full VecFetch、IVF+PQ+FlatStor、IVF+PQ+Lance、IVF+RQ+FlatStor、IVF+RQ+Lance、DiskANN+FlatStor 和 DiskANN+Lance。Full VecFetch 曲线必须保留全部有效 `nprobe` 点，包括在 QPS/recall Pareto 意义下被 dominated 的点，方便审计 sweep 形状。DiskANN 行必须使用全部 accepted raw-vector 参数组合，先取 QPS Pareto frontier；如果某条 DiskANN frontier 超过 8 个点，则按相近 recall 区间降采样，在每个区间内保留 QPS 最高的代表点，并保留最低/最高 recall 端点。严格 frontier 少于 5 个点时，保留全部 frontier 点，不补入被支配点。ImageNet1K IVF 图中只允许使用 `nlist=6400` 行。

## Risks / Trade-offs

- [Risk] COCO 或 MS MARCO adapter/query 对齐错误。  
  Mitigation: 增加 adapter gate，在调度数据集前检查 query count、ID policy 和 `gt_top100.npy` 行数。

- [Risk] runner 意外重建 VecFetch 索引。  
  Mitigation: 每个正式 VecFetch 运行都要求显式 `--index-dir`，并在目录缺失或不匹配时失败。

- [Risk] ImageNet1K 旧 `nlist=4096` IVF 行混入新对比。  
  Mitigation: 本 change 的聚合必须要求 ImageNet1K IVF 行使用 `nlist=6400`，并记录排除的历史行。

- [Risk] `fixed_vec_buffer_count=1024` 改变旧 probe 结果的解释。  
  Mitigation: 不把旧 probe 输出合并进正式结果面；所有新行必须包含 fixed-buffer metadata。

- [Risk] Full VecFetch 高 recall 点无法达到 `0.995`。  
  Mitigation: 保留 `best-effort` selection 行，而不是让运行失败；原始 Pareto 曲线仍是主要证据。

## Migration Plan

1. 为本 change 新增或扩展实验 runner，包含 accepted-index 验证和 adapter readiness 检查。
2. dry-run 完整命令矩阵，确认所有 VecFetch 命令都包含 `--index-dir`。
3. 每个数据集先跑 `topk=10,100`、`nprobe=64` smoke 点。
4. 按小到大顺序执行 Full VecFetch sweep：COCO、VoxCeleb2、Amazon ESCI、ImageNet1K、MS MARCO。
5. 执行 ImageNet1K IVF `nlist=6400` baseline 六点重跑。
6. 在专用输出根目录下聚合并验证 combined Pareto CSV。
7. 基于 verified combined CSV 生成五个数据集的 `Recall@10` 和 `Recall@100` Pareto 图表。
8. 写最终报告，记录图表路径、验证结果和资源状态。

## Open Questions

- MS MARCO 是否只用现有 200-query adapter 做 smoke，还是 apply 阶段必须在任何正式运行前物化新的 1000-query adapter。正式结果必须使用 1000 queries。
