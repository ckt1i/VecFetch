## Why

`Deep1M_synth` 计划作为论文 refine 阶段的第三个数据集，但当前代码库里只有原始 Deep1M 资产和旧版 payload 文件；旧版 payload 的生成规则不满足当前“合成数据集可控 workload”的论文口径。现有 formal-study runner 还不能直接加载 `deep1m_synth`，也不能为它构建或复用 canonical artifact，更不能跑出论文要求的 FlatStor 与 Lance baseline family。

这个 change 的目标是先把 `Deep1M_synth` 补成一等 formal dataset，再按现有主实验口径执行规划好的 `topk=10` / `topk=20` 实验矩阵。

## What Changes

- 在现有 formal-baseline 根目录下补齐 `Deep1M_synth` 数据合约，包括 embeddings、split、GT、payload backend 元数据以及 `bench_e2e` adapter。
- 增加 runner 对 `deep1m_synth` 的支持，明确执行口径使用 L2 metric、固定 1000-query split，并保证 `gt_top10/20/100` 对齐；同时在文档中注明：由于公开 Deep1M 已做 L2 归一化，本实验中的欧氏距离（L2）排序与余弦相似度排序等价。
- 尽可能复用已有 Deep1M 的 4096-cluster centroids / assignments，避免重新聚类，直接形成 canonical artifact。
- 重新生成 `Deep1M_synth` 的正式 synthetic payload，不再把 legacy `deep1m_flatstor.*` 作为 formal source of truth；默认 variant 使用确定性 `bucket_mixture_v1` 分布，payload size 最小 256B、目标平均 4KB、最大 64KB，并由 `cleaned/payload.parquet` 派生 FlatStor、Lance 和 Parquet backend。
- 把论文 baseline family 需要的两种 Deep1M_synth payload backend 都 formalize：
  - FlatStor：`payload_flatstor/default/index.npy` 和 `payload.dat`
  - Lance：`payload_lance/default`，包含 1,000,000 行，且表命名与当前 fetch 代码兼容
- 在正式实验前增加 smoke gate：资产 shape、split/GT 对齐、payload 行数、随机 payload fetch、canonical artifact provenance，以及单点 recall sanity。
- 执行正式的 Deep1M_synth 实验计划：
  - VecFetch / BoundFetch-Guarded `topk=10` sweep
  - `IVF+RaBitQ+FlatStor`
  - `IVF+PQ+FlatStor`
  - `IVF+RaBitQ+Lance`
  - `IVF+PQ+Lance`
  - `topk=20` supplement，只在 `topk=10` 和 `recall@20` 解析通过后执行
  - 对最终可见点做 repeat3 cleanup
- 生成论文可用的 CSV 汇总、matched-quality 选点结果和最终决策摘要。

## Capabilities

### New Capabilities

- `deep1m-synth-formal-data`: 定义 `Deep1M_synth` 所需的 formal 数据合约、adapter、payload backend readiness、canonical artifact readiness 和 smoke validation。
- `deep1m-synth-experiment-execution`: 定义 `Deep1M_synth` 所需的 `topk=10` / `topk=20` 实验矩阵、warmup/measurement 协议、baseline 完整性规则、cleanup repeats 和结果输出。

### Modified Capabilities

- 无。现有通用 benchmark 与 payload capability 不改 spec 语义；本 change 只为 `Deep1M_synth` 增加数据准备与实验执行要求。

## Impact

- 受影响脚本：
  - `/home/zcq/VDB/baselines/formal-study/legacy/scripts/prepare_deep1m_synth_assets.py`
  - `/home/zcq/VDB/baselines/formal-study/legacy/scripts/_shared/datasets.py`
  - `/home/zcq/VDB/baselines/formal-study/legacy/scripts/export_payload_backends/export_payload_backends.py`
  - `/home/zcq/VDB/baselines/formal-study/legacy/scripts/run_coco_canonical_build.py` 或一个新的 Deep1M 专用 canonical build entrypoint
  - `/home/zcq/VDB/baselines/formal-study/legacy/scripts/run_vector_search/*.py`
  - `/home/zcq/VDB/baselines/formal-study/legacy/scripts/run_e2e_coupled/*.py`
  - 任何新增的 Deep1M_synth orchestration / summarization 脚本
- 受影响数据根目录：
  - `/home/zcq/VDB/data/deep1m`
  - `/home/zcq/VDB/data/formal_baselines/deep1m_synth`
  - `/home/zcq/VDB/baselines/data/formal_baselines/deep1m_synth`
  - `/home/zcq/VDB/baselines/formal-study/outputs/index_build/deep1m_synth`
  - `/home/zcq/VDB/baselines/formal-study/outputs/deep1m_synth`
- 依赖：
  - Lance export 和 fetch validation 所使用的 Python 环境必须能导入 `lancedb`。当前 `labnew` 环境可用，默认 Python 不可用。
- 受影响系统：
  - formal-study 数据准备链路
  - VecFetch / `bench_e2e` canonical artifact 复用
  - IVF+PQ 与 IVF+RaBitQ baseline runner
  - 论文侧 summary CSV 生成
