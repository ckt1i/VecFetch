# Deep1M_synth 追加实验计划

**日期**: 2026-05-10  
**来源**: 复用 `refine-logs/thesis-minimal-experiment-2026-05-02` 的 thesis-minimal 方案，并作为 `paper-refine-round1-20260509_042859` 后的第三数据集追加计划。  
**目标**: 在不改变论文主线的前提下，把 `deep1m_synth` 加入主实验和 top-k 补充实验，用作规模与确定性合成 payload workload 控制变量。

**本次更正**: Deep1M_synth 的 baseline 口径必须与前文主实验一致；`IVF+PQ+FlatStor`、`IVF+RaBitQ+FlatStor`、`IVF+PQ+Lance`、`IVF+RaBitQ+Lance` 均为必跑 baseline，Lance 不再作为 optional appendix 项。

**度量说明**: 公开 Deep1M 向量已经做过 L2 归一化，因此本实验虽然继续按欧氏距离（L2）执行与记录，但在排序意义上它与余弦相似度等价。后文若写 `metric=L2`，表示实现口径与 runner/cache 对齐，而不是与 cosine 语义冲突。

**payload 更正**: `Deep1M_synth` 的正式 payload 不再复用 legacy `deep1m_flatstor.*` 作为 source of truth；default variant 改为确定性 `bucket_mixture_v1` 合成分布，平均约 4KB、最小 256B、最大 64KB，并由 `cleaned/payload.parquet` 统一派生 FlatStor、Lance 和 Parquet backend。

**执行收尾状态**: 本计划已经执行完成。最终执行口径以后续 OpenSpec tasks 为准：topk=10/topk=20 的最终可见结果直接来自 sweep/supplement measurement，不再执行统一 repeat3 cleanup；VecFetch 在每个 sweep family 前各做 1 次 warmup，baseline 不做额外 warmup。

## Gate Check

- **Final method thesis**: BoundFetch-Guarded / VecFetch 使用 RaBitQ error-bound 将候选划分为 SafeIn、SafeOut、Uncertain，并用动态 I/O 调度降低 matched-quality E2E latency。
- **Dominant contribution**: bound-guided selective fetch，而不是新增检索模型或新 baseline 家族。
- **Rejected complexity**: 不引入 graph ANN、DiskANN 主线、RAIR/重复聚类、CRC early stop 作为论文贡献。
- **Deep1M_synth 的角色**: 第三数据集与 payload 控制变量，增强外部有效性；如果结果或资产不稳定，可降级为 appendix/supplement，不阻塞 COCO + MS MARCO 主结论。
- **Frontier primitive**: 无。该实验不需要 LLM/VLM/Diffusion/RL。

## 资产现状

已存在原始 Deep1M 资产：

- `/home/zcq/VDB/data/deep1m/deep1m_base.npy`: 1,000,000 x 96
- `/home/zcq/VDB/data/deep1m/deep1m_query.npy`: 10,000 x 96
- `/home/zcq/VDB/data/deep1m/deep1m_gt_top10.npy`
- `/home/zcq/VDB/data/deep1m/deep1m_gt_top20.npy`
- `/home/zcq/VDB/data/deep1m/deep1m_gt_top100.npy`
- `/home/zcq/VDB/data/deep1m/deep1m_centroid_4096.fvecs`
- `/home/zcq/VDB/data/deep1m/deep1m_cluster_id_4096.ivecs`

formal manifest 已预留：

```text
dataset=deep1m_synth
nlist=4096
nprobe={16,32,64,128,256,512}
methods={faiss_ivfpq_refine, ivf_rabitq_rerank}
storage_backends={flatstor,lance}
queries=1000
topk=10
candidate_budget=100
```

当前缺口：

- `/home/zcq/VDB/data/formal_baselines/deep1m_synth/embeddings/` 尚未形成稳定 formal contract。
- `/home/zcq/VDB/baselines/data/formal_baselines/deep1m_synth/gt/` 尚未形成 `gt_top10/20/100.npy` 的 formal copy。
- payload export 脚本当前显式支持 `deep8m_synth`，但没有把 `deep1m_synth` 纳入 `_load_cleaned_table` 分支。
- 旧版 `deep1m_flatstor.*` / `deep1m_lance` 只能作为 legacy 对照或迁移参考，不能作为本轮正式 synthetic payload 来源。
- Lance backend 的 Deep1M_synth payload/index contract 需要与 FlatStor 同步确认，且两者必须都从新的 cleaned synthetic payload 派生；不能只用 FlatStor 代表前文 baseline family。
- BoundFetch canonical artifact 需要明确 builder、assignments、bench dataset adapter、GT 路径，避免复用错误 cache。

## 实验原则

1. Deep1M_synth 不扩大论文故事，只回答“规模更大、payload 可控时结论是否仍成立”。
2. Deep1M_synth 的 baseline 方法必须与前文主实验一致：`IVF+PQ+FlatStor`、`IVF+RaBitQ+FlatStor`、`IVF+PQ+Lance`、`IVF+RaBitQ+Lance` 均为必跑 baseline。
3. 最终执行时，VecFetch 在 topk=10 和 topk=20 sweep family 启动前各做 1 次 warmup；warmup 不入表。
4. 最终执行时，RaBitQ/PQ baseline 不做额外 warmup；最终入表点直接采用 formal sweep/supplement measurement。
5. 所有系统必须使用同一 query subset 和同一 GT；先做 recall smoke，再跑矩阵。
6. topk=20 只在 topk=10 sweep 和 matched-quality 点稳定后执行；若 topk=20 进入论文或补充表，也必须保持同一 baseline family，不单独删 Lance。

## 数据与参数合约

| 项 | 设置 |
|---|---|
| Dataset id | `deep1m_synth` |
| Raw source | `/home/zcq/VDB/data/deep1m` |
| Base vectors | 1,000,000 x 96 float32 |
| Query pool | 10,000 x 96 float32 |
| Formal query count | 1000 |
| Query selection | 使用 `sample_query_indices(10000, 1000)` 并持久化 split |
| Metric | L2（公开 Deep1M 已做 L2 归一化，故与余弦相似度排序等价） |
| nlist | 4096 |
| assignment | single |
| primary nprobe sweep | 16, 32, 64, 128, 256, 512 |
| optional nprobe | 768, 1024，仅当 512 达不到共同质量阈值 |
| bits | 4 |
| topk main | 10 |
| topk supplement | 20 |
| candidate_budget top10 | 100 |
| candidate_budget top20 | 150 first pass；必要时 200 |
| protocol | warm-serving, full_preload/resident where applicable |
| payload variant | `default` |
| payload distribution | `bucket_mixture_v1` |
| payload seed | `20260510` |
| payload size | min 256B，target mean 4KB，max 64KB |
| payload source of truth | `cleaned/payload.parquet`，再派生 FlatStor / Lance / Parquet |

Payload 建议：

- 主实验使用单一 `default` payload variant，避免让 payload size sweep 干扰第三数据集主结论。
- default variant 使用 deterministic bucket mixture，而不是复用 legacy `deep1m_flatstor.dat`：
  - 256B：45.000%
  - 1024B：25.000%
  - 4096B：18.000%
  - 16384B：9.922%
  - 65536B：2.078%
- 生成方式采用 exact-count seeded shuffle，保证 1,000,000 行上的 bucket counts、总字节数和样本 checksum 可复现；payload bytes 由 `dataset_id`、`row_id` 和 `payload_size` 确定。
- `cleaned/payload.parquet` 必须包含 `row_id`、`doc_id`、`payload_size` 和 `payload`，FlatStor、Lance、Parquet backend 都从这份 cleaned source 派生。
- payload size sensitivity 可作为 appendix 后续分支，不进入本次 P0。

## 实验矩阵

### D0: 资产与 runner 校验

**优先级**: P0  
**目的**: 确认 Deep1M_synth 能按 formal-study 方式被所有 runner 读取。

检查项：

- raw base/query/GT shape 与 dtype。
- formal embeddings/GT copy 或 symlink 是否存在。
- query subset 与 `gt_top10/20/100` 行严格对齐。
- cleaned synthetic payload 行数为 1,000,000，`payload_distribution_id=bucket_mixture_v1`，min/mean/max 与 bucket counts 符合 contract。
- payload FlatStor `index.npy` 与 `payload.dat` 必须由 cleaned synthetic payload 派生，行数为 1,000,000，随机样本 bytes 与 cleaned source 一致。
- Lance dataset/index 必须由同一 cleaned synthetic payload 派生，行数、doc id、payload size、payload bytes 与 FlatStor 对齐。
- canonical artifact 中 `dataset=deep1m_synth`、`nlist=4096`、`bits=4`、single assignment。
- RaBitQ/PQ index cache 的 meta 必须指向同一 canonical artifact，不允许只因 cache 文件存在就复用。

输出：

- `deep1m_synth_asset_manifest.json`
- `deep1m_synth_smoke_validation.md`

决策门：

- 若任一系统在 smoke 中 `recall@10` 接近 0，停止并修复 query/GT/cache。
- 若 payload backend 行数不对，先不跑 E2E，只允许 vector-search smoke。

### D1: Topk=10 主 sweep

**优先级**: P0  
**Claim**: C1，第三数据集 matched-quality E2E latency。

| 系统 | Backend | nprobe | topk | candidate_budget | repeats |
|---|---|---:|---:|---:|---:|
| VecFetch / BoundFetch-Guarded | integrated / FlatStor path | 16,32,64,128,256,512 | 10 | n/a | sweep 1 次；最终表采用 sweep measurement |
| IVF+RaBitQ+FlatStor | FlatStor | 16,32,64,128,256,512 | 10 | 100 | sweep 1 次；最终表采用 sweep measurement |
| IVF+PQ+FlatStor | FlatStor | 16,32,64,128,256,512 | 10 | 100 | sweep 1 次；最终表采用 sweep measurement |
| IVF+RaBitQ+Lance | Lance | 16,32,64,128,256,512 | 10 | 100 | sweep 1 次；最终表采用 sweep measurement |
| IVF+PQ+Lance | Lance | 16,32,64,128,256,512 | 10 | 100 | sweep 1 次；最终表采用 sweep measurement |

Lance 更正：

- Lance 是 Deep1M_synth 的必跑 baseline backend，与 FlatStor 一起构成前文一致的 baseline family。
- 不允许用 FlatStor-only 结果替代 Lance 结果；如果 Lance runner 或资产阻塞，应先把 Deep1M_synth 降级为未完成，而不是在主实验表中省略 Lance。

指标：

- `recall@10`
- avg/p50/p95/p99 E2E latency
- VecFetch: SafeIn/SafeOut/Uncertain、reranked candidates、payload-prefetched reads、missing payload fetches、bytes read、submit calls、io_wait
- Baseline: vector-search latency、payload fetch latency、bytes read、candidate recall

选点规则：

1. **共同阈值规则**: 取所有主线系统共同可达的最高稳定阈值，建议候选 `R@10 >= 0.95`；如果 PQ 达不到，可把 PQ 标成低质量 baseline，但 PQ 的 FlatStor/Lance 行仍需保留为完整 baseline 口径。
2. **窄带规则**: 在 VecFetch 与 RaBitQ 之间找 `|ΔR@10| <= 0.005`，失败时放宽到 `<=0.010`。
3. **Pareto 规则**: 若严格窄带失败，正文不使用单点加速，改用 recall-latency curve 说明。

输出：

- `deep1m_synth_main_sweep_top10.csv`
- `deep1m_synth_matched_quality_top10.csv`
- `deep1m_synth_recall_latency_curve_top10.csv`

### D2: Topk=20 补充实验

**优先级**: P1  
**Claim**: C4，top-k 放大下 selective fetch 是否稳定。

运行时机：

- D1 完成；
- `recall@20` 输出或后处理已验证；
- 已选出 top10 的低/中/高质量代表点。

建议点位：

- VecFetch: 选择 top10 sweep 中低/中/高三个点，初始可用 `nprobe=32,64,128`；若 Deep1M 的曲线偏低，改为 `64,128,256`。
- RaBitQ/PQ: 对齐 VecFetch 的三个点位附近，先用同 nprobe，再按 recall 补相邻点。

| 系统 | Backend | nprobe first pass | topk | candidate_budget | repeats |
|---|---|---:|---:|---:|---:|
| VecFetch / BoundFetch-Guarded | integrated | 32,64,128 | 20 | n/a | 1 warmup + 1 measurement；最终表采用 supplement measurement |
| IVF+RaBitQ+FlatStor | FlatStor | 32,64,128 | 20 | 150, fallback 200 | 1 measurement；最终表采用 supplement measurement |
| IVF+PQ+FlatStor | FlatStor | 32,64,128 | 20 | 150, fallback 200 | 1 measurement；最终表采用 supplement measurement |
| IVF+RaBitQ+Lance | Lance | 32,64,128 | 20 | 150, fallback 200 | 1 measurement；最终表采用 supplement measurement |
| IVF+PQ+Lance | Lance | 32,64,128 | 20 | 150, fallback 200 | 1 measurement；最终表采用 supplement measurement |

指标：

- `recall@20`
- avg/p95/p99 E2E latency
- bytes read
- reranked candidates
- payload fetch count

输出：

- `deep1m_synth_topk20_supplement.csv`
- `deep1m_synth_topk10_vs_topk20_summary.csv`

决策门：

- 如果 topk=20 仍显示 VecFetch 在相近 recall 下更低 E2E latency，可进入主文补充表。
- 如果 topk=20 结果只在部分质量区间成立，放 appendix。
- 如果 runner 不能稳定输出 `recall@20`，先完成后处理，不允许用 `recall@10` 代替。

### D3: 结果稳定性与 cleanup

**最终状态**: superseded by updated OpenSpec tasks。  
**说明**: 本轮不再执行统一 warmup/repeat cleanup；topk=10 正式结果直接来自 main sweep measurement，topk=20 正式结果直接来自 supplement measurement。该变更避免在已经完成完整 sweep 的情况下重复消耗实验时间，同时保留 warmup、smoke、formal 输出分目录，便于审计。

输出：

- `deep1m_synth_sweep_summary.csv`
- `DEEP1M_SYNTH_DECISION_SUMMARY.md`

## 执行顺序

1. D0 资产 formalization 与 smoke。
2. D1 topk=10 VecFetch + 四个 baseline 组合 sweep。
3. D1 matched-quality 选点。
4. D2 topk=20 三点补充，保持同一 baseline family。
5. 生成最终 CSV、decision summary 和结论文档。
6. 更新论文表格和 tracker。

## 最小可接受版本

如果时间有限，只跑：

1. `deep1m_synth` topk=10: VecFetch + `IVF+PQ+FlatStor` + `IVF+RaBitQ+FlatStor` + `IVF+PQ+Lance` + `IVF+RaBitQ+Lance`，nprobe=32,64,128,256。
2. topk=10 matched-quality 选点，覆盖最终可见的四个 baseline 组合。
3. topk=20: 若要报告，则 VecFetch + 四个 baseline 组合在 top10 选点及相邻点跑 2-3 个点；若预算不足，topk=20 整体后移，而不是只保留 FlatStor。

这足以支持一句保守结论：

> Deep1M_synth confirms the same matched-quality trend on a larger controlled-payload workload against the same IVF+PQ/RaBitQ x FlatStor/Lance baseline family, while the main claims remain grounded in COCO100K and MS MARCO.

## 不建议本轮追加

- 不跑 Deep8M_synth 作为第三主数据集，除非 Deep1M_synth formalization 阻塞。
- 不把 256B/4KB/64KB 做成多个独立 payload tier 主实验；本轮只使用一个包含多桶大小的 default synthetic distribution。
- 不把 Lance 从 Deep1M_synth baseline 中删除或降级为 optional。
- 不新增 DiskANN/HNSW/ScaNN。
- 不重跑 COCO/MS MARCO 已冻结消融，除非写作时发现必须统一表格口径。

## 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| `deep1m_synth` formal dataset 尚未落地 | runner 不能直接调度 | 先做 D0；必要时用 symlink/copy 建立 formal embeddings 与 GT |
| query subset 与 GT 不一致 | recall 接近 0，污染表格 | smoke 必须检查 recall 曲线单调性和非零 candidate recall |
| RaBitQ/PQ cache 复用错误 artifact | recall 曲线异常 | 每次 run 记录 canonical artifact；cache meta 必须校验 centroids/assignments |
| Lance Deep1M_synth backend 未 formalize | baseline 口径无法与前文一致 | D0 增加 Lance asset gate；未通过前不启动正式 sweep |
| legacy payload provenance 不可审计 | synthetic workload 口径不清 | 不再把 `deep1m_flatstor.*` 作为 source of truth；统一从 `bucket_mixture_v1` cleaned parquet 派生 |
| payload size 过大导致运行极慢 | 拖慢主线 | 主实验只用一个平均约 4KB 的 default payload；payload tier sweep 放 appendix |
| PQ recall 上限过低 | 无法共同阈值 matched-quality | PQ 作为低质量 baseline；VecFetch vs RaBitQ 用窄带或共同阈值 |
| topk=20 输出不完整 | 不能支持 C4 | 先实现/验证 recall@20 后处理，再正式跑 |
