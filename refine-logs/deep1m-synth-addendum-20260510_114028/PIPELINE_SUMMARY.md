# Pipeline Summary

**Problem**: 在现有 COCO100K + MS MARCO 论文实验基础上，追加第三个 `Deep1M_synth` 数据集，用更大规模和确定性 synthetic payload workload 验证 matched-quality E2E latency 趋势。公开 Deep1M 已做 L2 归一化，因此本实验中采用的欧氏距离（L2）与余弦相似度在排序上等价。  
**Final Method Thesis**: VecFetch / BoundFetch-Guarded 的核心仍是 bound-guided selective fetch 与动态 I/O 调度；Deep1M_synth 用作外部有效性与 top-k robustness 补充。  
**Final Verdict**: COMPLETE. D0 asset gate、canonical artifact、FlatStor/Lance backend、top-10 sweep、matched-quality selection、top-k=20 supplement 与最终汇总均已完成。  
**Date**: 2026-05-10

## Final Deliverables

- Plan: `refine-logs/deep1m-synth-addendum-20260510_114028/DEEP1M_SYNTH_EXPERIMENT_PLAN.md`
- Tracker: `refine-logs/deep1m-synth-addendum-20260510_114028/DEEP1M_SYNTH_EXPERIMENT_TRACKER.md`
- Conclusion: `refine-logs/deep1m-synth-addendum-20260510_114028/DEEP1M_SYNTH_EXPERIMENT_CONCLUSION.md`
- Output root: `/home/zcq/VDB/baselines/formal-study/outputs/deep1m_synth/`
- Summary CSV root: `/home/zcq/VDB/baselines/formal-study/outputs/deep1m_synth/summary/`

Final result files:

- `deep1m_synth_main_sweep_top10.csv`
- `deep1m_synth_recall_latency_curve_top10.csv`
- `deep1m_synth_matched_quality_top10.csv`
- `deep1m_synth_topk20_supplement.csv`
- `deep1m_synth_topk10_vs_topk20_summary.csv`
- `deep1m_synth_sweep_summary.csv`
- `DEEP1M_SYNTH_DECISION_SUMMARY.md`

## Execution Snapshot

- Data source: formal `deep1m_synth`, 1,000,000 base vectors, 1,000-query split, `metric=l2`.
- Payload source of truth: deterministic `bucket_mixture_v1`, seed `20260510`, min 256B, mean approximately 4KB, max 64KB.
- Backend completeness: FlatStor, Lance and Parquet were derived from the same `cleaned/payload.parquet`.
- Canonical artifact: `nlist=4096`, `bits=4`, single assignment, reused by VecFetch and IVF baselines.
- Top-10 sweep: VecFetch plus `IVF+PQ/RaBitQ x FlatStor/Lance`, nprobe `16,32,64,128,256,512`.
- Top-k=20 supplement: VecFetch plus the same four IVF baseline combinations, nprobe `32,64,128`.
- Warmup policy: one VecFetch warmup before each VecFetch sweep family; baseline formal measurements did not use extra warmup, matching the updated OpenSpec tasks.
- Cleanup repeat policy: superseded by updated OpenSpec tasks; final visible values come from sweep/supplement measurements rather than repeat3 cleanup.

## Main Results

Top-10 matched-quality selection used the common-threshold rule `R@10 >= 0.950`.

| System | Backend | nprobe | Candidate budget | R@10 | E2E ms |
|---|---|---:|---:|---:|---:|
| VecFetch | native | 32 | adaptive | 0.9540 | 1.253 |
| IVF+RaBitQ+FlatStor | flatstor | 64 | 100 | 0.9783 | 3.852 |
| IVF+PQ+FlatStor | flatstor | 128 | 100 | 0.9569 | 4.625 |
| IVF+RaBitQ+Lance | lance | 64 | 100 | 0.9783 | 17.083 |
| IVF+PQ+Lance | lance | 128 | 100 | 0.9569 | 17.018 |

Important interpretation:

- Under the common-threshold rule, VecFetch is the lowest-latency selected point once every system reaches `R@10 >= 0.950`.
- Against the closest RaBitQ+FlatStor quality region, the conclusion is more conservative but still positive: at nprobe 64, VecFetch has `R@10=0.9814` and `E2E=2.809 ms`, while IVF+RaBitQ+FlatStor has `R@10=0.9783` and `E2E=3.852 ms`.
- Lance baselines are complete but much slower on this workload, which mainly reflects backend payload fetch overhead rather than vector recall differences.

Top-k=20 supplement at nprobe 64:

| System | Backend | R@20 | E2E ms |
|---|---|---:|---:|
| VecFetch | native | 0.9761 | 1.931 |
| IVF+RaBitQ+FlatStor | flatstor | 0.9721 | 4.240 |
| IVF+PQ+FlatStor | flatstor | 0.9335 | 4.596 |
| IVF+RaBitQ+Lance | lance | 0.9721 | 21.469 |
| IVF+PQ+Lance | lance | 0.9335 | 20.237 |

Candidate budget `150` was sufficient for top-k=20; no fallback to `200` was needed.

## Claim Status

- C1-addendum is supported: on Deep1M_synth top-10, VecFetch preserves the matched-quality latency advantage against the full `IVF+PQ/RaBitQ x FlatStor/Lance` baseline family.
- C4-addendum is supported as a supplement: top-k=20 keeps the same qualitative trend, with VecFetch remaining faster at comparable RaBitQ recall regions.
- Deep1M_synth should be presented as third-dataset corroboration, not as the sole basis for the paper's main claim.

## Caveats

- The common-threshold table is not a strict same-recall table. It selects each system's fastest point above `R@10 >= 0.950`; use the nprobe-aligned comparison when arguing stricter recall proximity.
- Earlier setup failures exist in `RUN_STATUS.csv` for PQ default `m=64` and missing `lancedb`; those were corrected with `m=16, nbits=8` and `/home/zcq/anaconda3/envs/labnew/bin/python`. Failed/smoke/warmup rows are excluded from final CSVs.
- Extra exploratory VecFetch top-10 points at nprobe `48` and `96` are kept in the recall-latency curve table, but excluded from the required main sweep table.

## Next Action

- Use `DEEP1M_SYNTH_EXPERIMENT_CONCLUSION.md` and `DEEP1M_SYNTH_DECISION_SUMMARY.md` as the source for paper text.
- If the paper needs a stricter table, report both common-threshold selected points and the nprobe-aligned RaBitQ comparison to avoid overstating recall matching.
