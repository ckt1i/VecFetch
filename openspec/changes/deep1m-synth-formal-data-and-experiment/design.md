## Context

`Deep1M_synth` 的定位是为当前 `COCO100K + MS MARCO` 论文实验补上一个更大规模、synthetic payload 可控的第三数据集。公开 Deep1M 向量已经做过 L2 归一化，因此本实验执行时虽然继续使用 L2 距离口径，但其排序与余弦相似度排序等价。原始 Deep1M 资产已经存在于 `/home/zcq/VDB/data/deep1m`，包括 base/query vectors、`gt_top10/20/100`、4096 centroids 和 4096 assignments。

但 formal-study runner 链路还不能直接使用这个数据集。当前 `dataset_runnable_summary("deep1m_synth")` 会报 phase0 incomplete，`load_dataset_bundle("deep1m_synth")` 也会报 `Unsupported dataset`。C++ `bench_e2e` 仍然要求 COCO-style 输入契约：`image_embeddings.npy`、`image_ids.npy`、`query_embeddings.npy`、`query_ids.npy` 和 `metadata.jsonl`。现有 Python baseline runner 通过 `_shared.datasets.load_dataset_bundle` 取数据，而 payload fetch 则从 `/home/zcq/VDB/baselines/data/formal_baselines/<dataset>/payload_*` 读取。

当前可以复用的资产包括：

- `/home/zcq/VDB/data/deep1m` 下的原始 Deep1M vectors 和 GT
- `/home/zcq/VDB/data/deep1m` 下的 4096 centroids / assignments
- `/home/zcq/VDB/baselines/data/deep1m_flatstor.*` 与 `/home/zcq/VDB/baselines/data/deep1m_lance` 只能作为 legacy 对照或迁移参考，不能作为新的 formal payload source of truth
- `labnew` Python 环境可导入 `lancedb`，默认 Python 当前不可导入

这个 proposal 的核心，是复用 Deep1M 的向量、GT 和聚类资产，重新生成可审计的 synthetic payload，并把“数据 readiness”与“实验执行”都做成明确 gate，在结果进入论文前先把契约锁死。

## Goals / Non-Goals

**Goals:**

- 让 `deep1m_synth` 成为 formal-study 的一等数据集，执行口径固定为 L2 metric、1,000,000 条 base vectors、1,000-query split，并保证 `gt_top10/20/100` 对齐；同时明确说明因向量已做 L2 归一化，L2 与 cosine 在排序上等价。
- 在满足 formal contract 的前提下，复用已有 Deep1M raw vectors、GT、centroids 和 assignments；payload 则按新的 deterministic synthetic distribution 重新生成。
- 提供 `bench_e2e` adapter，使 VecFetch / BoundFetch-Guarded 可以为 `Deep1M_synth` 构建并复用 canonical artifact。
- 明确要求 FlatStor 和 Lance 都必须作为 `Deep1M_synth` baseline backend。
- 增加 validation gate，在 query/GT/cache/payload 契约错误时提前停止，避免写出误导性的 recall 结果。
- 执行并汇总已经规划好的 `Deep1M_synth topk=10` 与 `topk=20` 实验矩阵。

**Non-Goals:**

- 不把 `Deep8M_synth` 引入为第三主数据集。
- 不在这个 change 中跑 payload size tier sweep。
- 不新增 DiskANN、HNSW、ScaNN 或其他 graph ANN baseline family。
- 不重跑已经冻结的 COCO / MS MARCO 实验，除非为了保持表格 schema 一致而必须改动。
- 如果 `Deep1M_synth` 结果需要进入论文，不允许把 Lance 降为 optional。

## Decisions

### Decision 1: 把 Deep1M_synth 当作 formal dataset，而不是一次性的 legacy 资产

`Deep1M_synth` 数据将落到与 COCO / MS MARCO 相同的 formal 根目录：

- raw/formal vectors：`/home/zcq/VDB/data/formal_baselines/deep1m_synth/embeddings/`
- formatted assets：`/home/zcq/VDB/baselines/data/formal_baselines/deep1m_synth/`
- run outputs：`/home/zcq/VDB/baselines/formal-study/outputs/deep1m_synth/`
- canonical artifact：`/home/zcq/VDB/baselines/formal-study/outputs/index_build/deep1m_synth/...`

考虑过的替代方案：直接从 `/home/zcq/VDB/data/deep1m` 和旧 payload 路径启动实验。这样前期更快，但会重新引入 COCO 类似的 GT/path mismatch 风险，也会让 Lance / FlatStor provenance 难以审计。

### Decision 2: 固化 1000-query split，并由这个 split 派生所有 GT

formal split 将复用现有 deterministic `sample_query_indices(10000, 1000)` 约定，写入 `splits/split_v1.json`。formal GT 文件则直接从现有 Deep1M `gt_top10/20/100` 中抽取对应行，并统一转成 `int64`。

考虑过的替代方案：直接取前 1000 条 query。这样更简单，但会偏离现有采样约定，也更难及时发现 query/GT 错位。

### Decision 3: 为 Deep1M_synth 增加显式 L2 loader，并记录其与 cosine 的等价关系

`_shared.datasets.load_dataset_bundle` 需要增加一个显式的 `Deep1M_synth` 分支。这个分支必须返回 `metric="l2"`，使用 formal split，并保留 `0..999999` 作为 row ids。文档与 manifest 还必须注明：公开 Deep1M 已经做 L2 归一化，因此这里保留 `metric="l2"` 是实现与缓存兼容要求，而对应的近邻排序与 cosine similarity 等价。

考虑过的替代方案：把 `Deep1M_synth` 强行接到 `_load_npy_dataset`。这个路径默认走 cosine 语义，对 Deep1M 来说是错误的。

### Decision 4: 直接复用已有 4096 centroids / assignments 作为 canonical coarse artifact

canonical artifact 优先直接复用现有 Deep1M 4096 文件：

- centroids：`/home/zcq/VDB/data/deep1m/deep1m_centroid_4096.fvecs`
- assignments：`/home/zcq/VDB/data/deep1m/deep1m_cluster_id_4096.ivecs`

artifact manifest 必须记录 `dataset=deep1m_synth`、`nlist=4096`、`bits=4`、`assignment_mode=single`、`metric=l2`，以及 adapter 和 bench index 的路径。如果 `bench_e2e` 需要生成自己的 index 目录，也必须使用这组 centroids / assignments，而不是重新聚类。

考虑过的替代方案：改用 hierarchical SuperKMeans 变体。这样会引入新的变量，违背“优先用已经准备好的 Deep1M 资产”的目标。

### Decision 5: 以 deterministic bucket-mixture 重新生成 synthetic payload

`Deep1M_synth` 的 default payload variant 不再复用 legacy `deep1m_flatstor.*` 作为正式来源，而是由确定性分布生成 `cleaned/payload.parquet`，再从同一个 cleaned source 导出 FlatStor、Lance 和 Parquet backend。

固定参数如下：

- `payload_distribution_id=bucket_mixture_v1`
- `payload_seed=20260510`
- `min_bytes=256`
- `target_mean_bytes=4096`
- `max_bytes=65536`
- size buckets：
  - 256B：45.000%
  - 1024B：25.000%
  - 4096B：18.000%
  - 16384B：9.922%
  - 65536B：2.078%

生成策略采用 exact-count seeded shuffle，而不是对每行独立 IID 抽样：先按 1,000,000 行计算每个 bucket 的行数，再用固定 seed 打乱 bucket assignment，使总字节数和分布在不同机器上稳定复现。payload 内容只由 `(dataset_id, row_id, payload_size)` 决定，例如重复 `sha256("deep1m_synth:<row_id>")` 的 digest 并截断到目标长度。

`cleaned/payload.parquet` 必须至少包含：

- `row_id: int64`
- `doc_id: int64`
- `payload_size: int64`
- `payload: large_binary`

FlatStor 的 `payload_flatstor/default/index.npy` 与 `payload.dat`、Lance 的 `payload_lance/default`、以及 Parquet backend 都必须由这份 cleaned parquet 派生。manifest 需要记录 distribution id、seed、bucket counts、actual min/mean/max、total payload bytes 和 sample checksum。

考虑过的替代方案：继续复用 legacy FlatStor / Lance payload。这个方案前期更快，但无法解释 legacy payload 的分布规则，也会让 `Deep1M_synth` 的“可控 synthetic payload workload”变成不可审计资产，因此不再作为 formal source。把 Lance 放进 appendix 也已经被实验计划否决，因为 `Deep1M_synth` 的 baseline 口径必须与现有论文主实验一致。

### Decision 6: 把执行链路拆成 validation、sweep、cleanup、summarize

执行路径建议分为几个阶段：

1. `validate`：检查数据 contract、runner 支持、payload fetch、canonical artifact 和单点 smoke
2. `topk10`：VecFetch + 四个 baseline 组合的完整 nprobe sweep
3. `select`：matched-quality 选点和 Pareto curve 生成
4. `topk20`：仅在 `topk=10` 与 `recall@20` 解析通过后执行补充实验
5. `cleanup`：对最终点做 `1 warmup + repeat3`
6. `summarize`：输出 CSV 和 `DEEP1M_SYNTH_DECISION_SUMMARY.md`

考虑过的替代方案：继续手工拼命令跑。这个方式很容易把 warmup、provenance 和失败 gate 跑乱，也不利于稳定复现。

## Risks / Trade-offs

- [风险] formal GT 行与 query split 不一致
  规避：先持久化 split，再由 split 派生 GT，并在正式实验前做 row-level recall smoke。
- [风险] `Deep1M_synth` 的度量说明与实现口径混淆
  规避：加显式 loader 分支，并在 validation 中强校验 metric=`l2`；同时在实验文档中明确写出“公开 Deep1M 已做 L2 归一化，因此 L2 与 cosine 排序等价”。
- [风险] 已经生成过的 legacy-based payload/backend 文件与新分布不一致
  规避：追加 synthetic payload remediation 任务，重新生成 `cleaned/payload.parquet`、FlatStor、Lance 和 Parquet backend，并在 validation 中强校验 `payload_distribution_id=bucket_mixture_v1`。
- [风险] 4KB 平均 payload 仍会产生约 4GB payload bytes，导出 Lance / Parquet 可能较慢
  规避：保留单一 default variant，不做 tier sweep；生成脚本采用批量流式写入，并在 manifest 中记录总字节数和导出耗时。
- [风险] payload size 分布在多次生成间漂移
  规避：使用 exact-count seeded shuffle，validation 检查 bucket counts、min/max/mean、payload_size checksum 和样本 bytes checksum。
- [风险] Lance 表名不匹配（`deep1m` vs `deep1m_synth`）
  规避：在 smoke 通过前完成 formal Lance 重导出或显式 alias 兼容。
- [风险] 默认 Python 无法导入 `lancedb`
  规避：明确使用 `labnew` Python 执行 Lance export / fetch validation，或者把 runner 环境显式钉死。
- [风险] 只复用 centroids / assignments 而没形成可用 bench index，会导致 VecFetch 仍然不可跑
  规避：canonical artifact validation 必须包含真实可用的 `bench_index_dir` 和一次 `bench_e2e` smoke run。
- [风险] PQ recall 达不到共同质量阈值
  规避：保留 PQ 结果作为 baseline completeness 的一部分，并在 matched-quality 选择中允许 VecFetch vs RaBitQ 走 narrow-band 规则。
