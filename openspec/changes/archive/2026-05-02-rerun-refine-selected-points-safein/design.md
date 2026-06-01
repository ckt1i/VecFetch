## Context

第一轮补实验目标是将主线聚焦为“量化误差界驱动的候选访问控制”。当前主要风险不在能否跑通，而在论文是否采用了清晰可复核的 matched-quality 选点，以及 SafeIn 消融是否足够稳定以支持保守结论。

`thesis-minimal-main-sweep` 已提供了可复用的执行基线：

- 仅保留 `coco_100k`、`msmarco_passage` 两个数据集；
- 使用 canonical artifact 复用，不对每个方法单独重训聚类；
- 正式测量使用 strict recall 且 `skip_gt=0`；
- VecFetch/BoundFetch 主线固定 `crc=1`、`early-stop=0`、`bits=4`；
- 严格区分 warmup 与正式 measurement。

复盘旧 COCO 的 RaBitQ 结果后可见，“同 `nprobe`”不等价：旧流程里 `IVF+RaBitQ+FlatStor` 重排候选被固定在 `candidate_budget=100`，而 VecFetch 走的是非 SafeOut 自适应候选重排集合。因此本轮补跑把 `candidate_budget` 作为 baseline 对齐核心参数。

## Goals / Non-Goals

**Goals:**

- 在 COCO 重跑主表选点时，对 VecFetch 与 `IVF+RaBitQ+FlatStor` 同时考虑 `nprobe` 和 `candidate_budget`。
- 保持 `thesis-minimal-main-sweep` 的 artifact 复用和测量协议，确保新旧结果可比。
- SafeIn 实验执行为每组先 1 次 warmup，再各测量 3 次。
- 输出采用重复分布统计（mean/median/std/min/best），不再以单次最佳值说话。
- 论文结论保持保守：主机制仍是 SafeOut 与 Uncertain，SafeIn 做边界化说明。

**Non-Goals:**

- 不新增图索引主线、新数据集、后端变体、GPU 并发或 top-k 压力实验。
- 不改 `thesis-minimal-main-sweep` 的 canonical 构建约束。
- 除非重复分布强支持，不把 SafeIn 写成当前主收益。
- 不将私有路径直接写进论文主文。

## Decisions

### 1. COCO 选点重跑按操作点组合执行，不再只看 `nprobe`

COCO 重跑将固定：

- 数据集：`coco_100k`
- queries：1000
- top-k：10
- canonical artifact：`faiss_kmeans`、`nlist=2048`、`bits=4`、single-assignment
- VecFetch：`nprobe ∈ {64, 128, 256}`
- `IVF+RaBitQ+FlatStor`：`nprobe ∈ {64, 128, 256}` 与 `candidate_budget ∈ {100, 256, 512}` 的组合
- 仅在必要时补测 `nprobe ∈ {96, 160, 192}`

`candidate_budget=256` 对齐了 VecFetch 在 `nprobe=64` 下的典型重排规模，`512` 用于饱和检查。

备选方案是仅保留 `candidate_budget=100`，但不能解决已识别的 recall 对齐偏差，无法回答 reviewer 的核心质疑。

### 2. 选点策略：结果落地后优先使用几乎完全 recall 对齐点

执行结果显示，`coco_main_alignment_round1.csv` 中已有几乎完全 recall 对齐的高召回点，可直接回答 reviewer 对 COCO 主表 recall 差距的质疑：

- `VecFetch`: `nprobe=128`, `R@10=0.9835`, `avg=1.8585 ms`, `p99=2.5807 ms`
- `IVF+RaBitQ+FlatStor`: `nprobe=128`, `R@10=0.9837`, `avg=2.4706 ms`, `p99=3.6518 ms`

因此论文主表推荐采用该 `nprobe=128` exact-recall pair。其 recall 差仅约 `0.0002`，平均延迟加速约 `1.33x`，p99 加速约 `1.41x`。

共同阈值规则和窄带规则仍作为审计性选择规则保留：

- 中等召回/Pareto 叙述可使用 `nprobe=64` 窄带点：`VecFetch R@10=0.9582` 对比 `IVF+RaBitQ+FlatStor R@10=0.9607`，平均延迟加速约 `1.85x`，p99 加速约 `1.70x`。
- 共同阈值 `R@10 >= 0.970` 可作为附加 sanity check，但不再优先于 `nprobe=128` 的近乎同 recall 对齐点。

### 3. SafeIn 重复固定为 1 次 warmup + 3 次测量

本轮 SafeIn 重复为：

- COCO：`topk=10`、`nprobe=64`，变体为 Full 与 SafeIn-off；
- MS MARCO：`topk=10`、`nprobe=128`，变体为 Full 与 SafeIn-off；
- 每组先完成 1 次 warmup，之后每个变体各做 3 次正式测量；
- warmup 仅留痕，不参与统计；3 次测量输出 mean/median/std/min/best。

### 4. Baseline 清理仅重跑论文最终点

COCO matched-quality 选择后，再对 `IVF+RaBitQ+FlatStor` 的论文最终点进行清理重跑；如 MS MARCO 最终表仍保留 `nprobe=128`，同步清理。

### 5. SafeIn 结果解释：当前 top-10 下不显著

SafeIn 重复结果进入以下论文门控：

- COCO: Full 与 SafeIn-off 的重复分布几乎重合，median avg 分别为 `1.1981 ms` 与 `1.1985 ms`，SafeIn prefetch 仅约 `0.109/query`。判定为当前 top-10 下不显著。
- MS MARCO: SafeIn-off 存在明显 I/O tail outlier，稳定子集与 Full 差异不足以支持 SafeIn 收益。判定为不显著且受尾延迟噪声影响。
- 论文中 SafeIn 只能写作“误差界约束下的可控整记录预取路径”和“未来适用边界”，不得写成当前主收益来源。

### 6. `candidate_budget=256/512` 扫描退出本轮闭环

Round 1 尝试扩展 RaBitQ `candidate_budget=256/512` 时暴露 COCO 输入/GT 合约不匹配问题。该问题与本轮 reviewer concern 的最小修复路径无关，因为 `nprobe=128` 的几乎同 recall 对齐点已经足以修复 COCO 主表质疑。

本轮将 `candidate_budget=256/512` 标记为 blocked/out-of-scope。若后续仍要比较更大 baseline rerank budget，应新开 change，先恢复 COCO 数据/GT 合约，再单独执行预算扫。

## Risks / Trade-offs

- [RaBitQ runner 不支持全部 `candidate_budget`] -> 明确记录 unsupported case，不塞进论文可比数据。
- [3 次重复仍有波动] -> 用 median+std 抑制单次偶然，避免过强结论。
- [COCO 阈值规则选点可能偏慢] -> 优先可解释性与保守性。
- [补充 `nprobe` 增加运行量] -> 仅在阈值/窄带规则无法落地时执行。
- [现有脚本可能按单一 `nprobe` 分组] -> 统一使用 `candidate_budget` 作为汇总维度之一。

## Migration Plan

1. 复用已存在的 COCO 与 MS MARCO canonical artifact；若溯源校验失败，先停止并修正。
2. 先跑 SafeIn 重复块，快速关闭稳定性问题。
3. 跑 COCO matched-quality 网格并记录 `candidate_budget`。
4. 以 `coco_main_alignment_round1.csv` 中 warm-rerun 后的 `nprobe=128` exact-recall pair 作为推荐论文主表点；`nprobe=64` 窄带点作为 Pareto/中等召回补充。
5. 将 SafeIn 结论更新为当前 top-10 下不显著，并把 SafeIn 压力测试保留为未来可选项。
6. 将 `candidate_budget=256/512` 标记为 blocked/out-of-scope；不作为本轮论文更新前置条件。
7. 产出 Round 1 CSV、重复汇总与决策说明。
