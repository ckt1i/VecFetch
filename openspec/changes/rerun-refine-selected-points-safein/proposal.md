## Why

第一轮论文评审暴露了两个高风险问题：COCO 主表的 `recall` 对齐不足，和 SafeIn 消融实验稳定性不足。重跑前必须先解决这两个问题。旧的“同 `nprobe` 对比”也有关键协议不等价：`IVF+RaBitQ+FlatStor` 被固定在 `candidate_budget=100`，而 VecFetch 使用的是非 SafeOut 自适应候选重排集合，导致质量可比性不成立。

## What Changes

- 基于 `thesis-minimal-main-sweep` 的 canonical artifact 与 warmup+measurement 协议，补充一轮 Round 1 的 COCO 匹配质量选点重跑。
- 要求 `IVF+RaBitQ+FlatStor` 重跑同时记录并在需要时扫 `candidate_budget`，不再只按 `nprobe` 对齐结论。
- 采用轻量 SafeIn 重复协议：
  - 每个数据集/变体组先跑 1 次 warmup；
  - 每个 Full 与 SafeIn-off 变体各进行 3 次正式重复测量。
- 输出按重复分布统计（mean/median/std/min/best），避免论文只引用单次最优。
- 除非另行排期，暂不包含 SafeIn 压力扩展测试。

## Result Update: 2026-05-09

本 change 的核心补实验已经产出。后续论文计划应以这些文件为准：

- `safein_repeat_coco_round1.csv`
- `safein_repeat_msmarco_round1.csv`
- `coco_main_alignment_round1.csv`
- `baseline_measurement_cleanup_round1.csv`

关键决策如下：

- COCO 主结果的 reviewer concern 已可关闭。`coco_main_alignment_round1.csv` 中存在几乎完全 recall 对齐的高召回点：`VecFetch nprobe=128, R@10=0.9835, avg=1.8585 ms, p99=2.5807 ms` 对比 `IVF+RaBitQ+FlatStor nprobe=128, R@10=0.9837, avg=2.4706 ms, p99=3.6518 ms`。该点应作为论文主表的推荐 COCO 对齐点，平均延迟加速约 `1.33x`，p99 加速约 `1.41x`。
- `nprobe=64` 窄带点可作为 Pareto/中等召回区间补充：`VecFetch R@10=0.9582, avg=1.2400 ms, p99=1.8126 ms` 对比 `IVF+RaBitQ+FlatStor R@10=0.9607, avg=2.2919 ms, p99=3.0884 ms`，平均延迟加速约 `1.85x`，p99 加速约 `1.70x`。
- SafeIn 重复实验显示 SafeIn 在当前 top-10 工作负载下不显著。COCO 上 Full 与 SafeIn-off 的 median avg 分别为 `1.1981 ms` 与 `1.1985 ms`；MS MARCO 上差异受 I/O 尾延迟抖动影响，不应写作 SafeIn 稳定收益。
- `candidate_budget=256/512` 的 RaBitQ COCO 扫描因 COCO 输入/GT 合约不匹配被明确阻塞。本轮不再要求其作为论文更新前置条件；如后续仍需要，应新开 change 恢复数据/GT 合约后再跑。
- 旧的中间文件和 `ROUND1_DECISION_SUMMARY.md` 中出现过 VecFetch `nprobe=128, latency=5.1991 ms` 的异常/旧值；论文与后续计划必须采用 warm-rerun 后的 `coco_main_alignment_round1.csv`。

## Capabilities

### New Capabilities
- `round1-refine-rerun-execution`：定义 Round 1 的补实验执行矩阵、选点、`candidate_budget` 对齐规则与 SafeIn 重复协议。
- `round1-refine-rerun-tracking`：定义输出文件、来源溯源字段、重复统计与论文可采信条件。

### Modified Capabilities

本次不修改现有 benchmark 或主线 sweep 的基础能力条款，仅新增一组可复用的 follow-up 实验合同。

## Impact

- 影响计划文件：`refine-logs/paper-refine-round1-20260509_042859/EXPERIMENT_PLAN.md` 与 `EXPERIMENT_TRACKER.md`。
- 复用 `openspec/changes/thesis-minimal-main-sweep/` 中的既有约束，尤其是 canonical index 复用、strict recall、warmup+measurement 语义。
- 预期新增输出文件为 `coco_main_alignment_round1.csv`、`safein_repeat_coco_round1.csv`、`safein_repeat_msmarco_round1.csv`、`baseline_measurement_cleanup_round1.csv`。
- 不涉及公开 benchmark API 或存储格式变更。
- 论文更新侧的直接影响是：第 7 章 COCO 主表可改用 `nprobe=128` 精确对齐点；SafeIn 小节和结论应降级为“不显著/机制边界”；摘要与结论不应把 SafeIn 写成当前性能收益来源。
