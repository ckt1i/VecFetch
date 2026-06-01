# Deep1M_synth 实验结论

**日期**: 2026-05-10  
**状态**: 完成。OpenSpec change `deep1m-synth-formal-data-and-experiment` 的 55 个 tasks 已全部完成，`openspec validate --strict` 通过。

## 实验定位

Deep1M_synth 是在 COCO100K 与 MS MARCO 之外追加的第三个数据集，用来验证更大规模、可控 synthetic payload workload 下，VecFetch / BoundFetch-Guarded 的 matched-quality E2E latency 趋势是否仍然成立。

它不改变论文主线：核心贡献仍是 bound-guided selective fetch 和动态 I/O 调度；Deep1M_synth 的作用是外部有效性和 top-k robustness 补充。

## 数据与执行口径

- Dataset: `deep1m_synth`
- Base vectors: 1,000,000 x 96
- Query split: 1000 queries
- Metric: L2。公开 Deep1M 向量已 L2 归一化，因此 L2 距离排序与 cosine similarity 排序等价。
- Payload: deterministic `bucket_mixture_v1`
- Payload seed: `20260510`
- Payload size distribution: min 256B, mean approximately 4KB, max 64KB
- Canonical artifact: `nlist=4096`, `bits=4`, single assignment
- Baseline family: `IVF+PQ/RaBitQ x FlatStor/Lance`

## 结果产物

最终 CSV 与决策摘要位于：

```text
/home/zcq/VDB/baselines/formal-study/outputs/deep1m_synth/summary/
```

主要文件：

- `deep1m_synth_main_sweep_top10.csv`
- `deep1m_synth_recall_latency_curve_top10.csv`
- `deep1m_synth_matched_quality_top10.csv`
- `deep1m_synth_topk20_supplement.csv`
- `deep1m_synth_topk10_vs_topk20_summary.csv`
- `deep1m_synth_sweep_summary.csv`
- `DEEP1M_SYNTH_DECISION_SUMMARY.md`

## Top-10 结论

top-10 的 matched-quality 选点采用 common-threshold 规则：`R@10 >= 0.950`。该规则成功，没有触发 narrow-band fallback。

| System | Backend | nprobe | Candidate budget | R@10 | E2E ms |
|---|---|---:|---:|---:|---:|
| VecFetch | native | 32 | adaptive | 0.9540 | 1.253 |
| IVF+RaBitQ+FlatStor | flatstor | 64 | 100 | 0.9783 | 3.852 |
| IVF+PQ+FlatStor | flatstor | 128 | 100 | 0.9569 | 4.625 |
| IVF+RaBitQ+Lance | lance | 64 | 100 | 0.9783 | 17.083 |
| IVF+PQ+Lance | lance | 128 | 100 | 0.9569 | 17.018 |

直接结论：

- 在共同阈值 `R@10 >= 0.950` 下，VecFetch 是最低延迟点。
- 与 FlatStor 上的 RaBitQ 相比，VecFetch 的 threshold-selected E2E latency 约为 `1.253 ms`，RaBitQ+FlatStor 为 `3.852 ms`，VecFetch 更快约 `3.07x`。
- 与 PQ+FlatStor 相比，VecFetch 更快约 `3.69x`。
- Lance baseline 的 recall 与 FlatStor 对齐，但 E2E latency 明显更高，说明该 workload 下 Lance payload fetch overhead 是主要成本之一。

更保守的近召回解释：

- common-threshold 不是严格 same-recall 表，它选择每个系统超过阈值后的最低延迟点。
- 若使用 nprobe-aligned / 近召回比较，VecFetch 仍然更快，但优势幅度更保守：
  - VecFetch nprobe=64: `R@10=0.9814`, `E2E=2.809 ms`
  - IVF+RaBitQ+FlatStor nprobe=64: `R@10=0.9783`, `E2E=3.852 ms`
  - 在这个更接近的质量区间，VecFetch 仍快约 `1.37x`。

## Top-k=20 结论

top-k=20 supplement 使用 nprobe `32,64,128`。在 baseline 上 `candidate_budget=150` 已足够，没有补测 `200`。

nprobe=64 的代表结果：

| System | Backend | R@20 | E2E ms |
|---|---|---:|---:|
| VecFetch | native | 0.9761 | 1.931 |
| IVF+RaBitQ+FlatStor | flatstor | 0.9721 | 4.240 |
| IVF+PQ+FlatStor | flatstor | 0.9335 | 4.596 |
| IVF+RaBitQ+Lance | lance | 0.9721 | 21.469 |
| IVF+PQ+Lance | lance | 0.9335 | 20.237 |

直接结论：

- top-k 从 10 增加到 20 后，VecFetch 的趋势仍然稳定。
- 在与 RaBitQ+FlatStor 接近的 `R@20` 区间，VecFetch 保持更低 E2E latency：`1.931 ms` vs `4.240 ms`，约 `2.20x` 更快。
- PQ 的 top-k=20 recall ceiling 明显低于 RaBitQ 与 VecFetch，因此更适合作为低质量 baseline，而不是 matched-quality 主对照。

## 论文写法建议

可采用的保守表述：

> On Deep1M_synth, VecFetch preserves the matched-quality latency advantage on a larger controlled-payload workload. At the common `R@10 >= 0.950` threshold, VecFetch selects a 1.253 ms operating point, while the strongest FlatStor RaBitQ baseline selects 3.852 ms. Under a stricter nprobe-aligned comparison near `R@10≈0.98`, VecFetch remains faster, at 2.809 ms versus 3.852 ms.

中文解释：

> Deep1M_synth 的结果支持论文主结论：在更大规模且 payload 分布可审计的场景中，VecFetch 仍能在相近或共同阈值质量下显著降低端到端延迟。这个收益不是来自换 baseline 或降低质量，而是来自 selective fetch 减少不必要 payload 读取和调度开销。

## Caveats

- 不应把 common-threshold 结果说成完全同 recall。它是“共同质量阈值下的最低延迟点”。
- 严格同 recall/近 recall 时，应引用 nprobe=64 的 VecFetch vs RaBitQ+FlatStor 对比。
- failed/smoke/warmup 输出已保留用于审计，但没有进入论文 CSV。
- Deep1M_synth 的结论应作为第三数据集补充，与 COCO100K 和 MS MARCO 的主实验共同支撑论文，而不是单独替代主实验。
