# Round 1 补充实验计划

**日期**: 2026-05-09  
**目标**: 回应第一版批注中实验可信度相关问题，优先修复主实验质量对齐和 SafeIn 消融稳定性。

## 实验总原则

1. 不扩大论文主线。补实验只围绕量化初筛路径、matched-quality 对齐和 SafeIn 稳定性。
2. 不只取 best。重复实验应报告 median/mean/std/min/best；正文可用 median 或稳定代表值，best 可放附录。
3. 不改变已冻结数据集。仍使用 COCO100K 与 MS MARCO Passage；不新增图索引或新数据集。
4. 不把私有路径写进论文。运行记录可以留在 refine log，正文/附录只写公开复现口径。

## E1: COCO 主结果 matched-quality 对齐重测

**优先级**: P0  
**回应批注**: “实验 7.1 主实验对齐选的点 recall 差的有点大，可能需要重跑。”  
**目标**: 让 COCO 主结果表的 VecFetch 与 IVF+RaBitQ FlatStor 处在更清晰的同质量区间。

### 设计

- 数据集: COCO100K
- top-k: 10
- queries: 1000
- 系统:
  - `VecFetch`
  - `IVF+RaBitQ + FlatStor`
  - 可选保留 `IVF+PQ + FlatStor` 作为低质量参考
- `nprobe`:
  - 必跑: 64, 128, 256
  - 若已有点不能接近，补跑: 96, 160, 192
- 协议:
  - warmup + measurement 分离
  - 每个候选主表点至少 3 次 measurement
  - 使用同一查询集、同一 ground truth、同一 full-preload/resident 口径

### 选点规则

主表采用阈值式 matched-quality，二选一：

1. **共同阈值规则**: 选择每个系统满足 `R@10 >= 0.970` 的最低 median avg latency 点。
2. **窄带规则**: 选择 `R@10` 差距不超过 0.005 或 0.010 的最低延迟点；若无法满足，正文说明无法找到严格窄带点，并使用完整 Pareto 图展示趋势。

推荐优先使用共同阈值规则，因为它更容易解释，且不会惩罚 recall 更高的系统。

### 输出

- `coco_main_alignment_round1.csv`
- COCO recall@10--latency/QPS Pareto 图更新数据
- 主表候选点说明: 系统、nprobe、R@10、avg/p95/p99、QPS、重复次数、median/std

### 决策门

- 若 VecFetch 在共同阈值下仍更快: 保留主结论，更新加速数字。
- 若 VecFetch 只在更高 recall 点更快: 改写为“保守高质量代表点”，不使用严格 matched-quality 加速。
- 若 VecFetch 在严格对齐后收益显著变小: 主文降低 COCO 加速权重，把 MS MARCO 和机制消融作为主要证据。

## E2: COCO SafeIn 稳定性重复实验

**优先级**: P0  
**回应批注**: “SafeIn 关闭后反倒比开了还慢/更快，目前判断是实验偶然因素，需要多跑几次取最好的值。”  
**目标**: 判断 SafeIn-off 比完整策略更快是否为偶然，确定正文应该如何描述 SafeIn。

### 设计

- 数据集: COCO100K
- top-k: 10
- nprobe: 64
- 变体:
  - Full
  - SafeIn-off
- repeats: 至少 5 次，建议 10 次
- 指标:
  - recall@10
  - avg/p50/p95/p99
  - SafeIn prefetch/query
  - final original-data fetch/query
  - bytes read
  - reranked candidates
  - submit calls / io wait

### 输出

- `safein_repeat_coco_round1.csv`
- 表: Full vs SafeIn-off 的 mean/median/std/min/best
- 图: avg 和 p99 的 repeat distribution

### 决策门

- 若 Full 与 SafeIn-off 差异小于运行方差: 正文写“SafeIn 在当前 top-10 下影响不显著”。
- 若 SafeIn-off 稳定更快: 正文写“SafeIn 预取触发少，当前设置下可能带来轻微额外 I/O；不是主收益来源”。
- 若 Full 稳定更快: 可保留“SafeIn 有小幅辅助作用”，但仍不能超过 SafeOut/Uncertain 的叙事权重。

## E3: MS MARCO SafeIn 稳定性确认

**优先级**: P1  
**目标**: 检查 SafeIn 在大原始数据文本 workload 下是否同样只是次要机制。

### 设计

- 数据集: MS MARCO Passage
- top-k: 10
- nprobe: 128
- 变体:
  - Full
  - SafeIn-off
- repeats: 3-5 次
- 指标同 E2。

### 决策门

- 若 MS MARCO 也无明显收益: 结论中统一写 SafeIn 当前触发少。
- 若 MS MARCO 有小幅收益: 只写成 workload-specific 观察，不推广到 COCO。

## E4: 基线协议清理重测

**优先级**: P1  
**目标**: 消除 `measurement` 与 `warm_coupled` 混用带来的质疑。

### 设计

只对最终主表会用到的 IVF+RaBitQ FlatStor 点重跑：

- COCO: 选定 `nprobe` 点及其相邻点
- MS MARCO: nprobe=128
- 协议: 独立 warmup + measurement
- repeats: 3 次

### 决策门

- 如果重测结果与现有 `warm_coupled` 接近: 第 6 章可简化协议说明，附录保留兼容说明。
- 如果差异明显: 主表改用新 measurement 结果，旧结果不再进入正文。

## E5: SafeIn 适用边界压力实验

**优先级**: P2，可选  
**目标**: 如果时间允许，探索 SafeIn 在更大返回集合或更大原始数据下是否会变得有意义。

### 设计 A: top-k 放大

- 数据集: COCO100K
- top-k: 20, 50
- 变体: Full vs SafeIn-off
- nprobe: 64 或最终主表附近点

### 设计 B: SafeIn threshold sweep

- 数据集: COCO100K 或 MS MARCO
- top-k: 10
- threshold: disabled, 256 KiB, large
- 目标: 观察 SafeIn 预取触发数、无效预取和 p99 的关系。

### 决策门

- 若找到 SafeIn 有效区间: 放在讨论或附录，作为未来适用边界。
- 若仍无收益: 不强行保留压力实验，正文只写当前 top-10 下 SafeIn 触发少。

## 推荐执行顺序

1. E2 COCO SafeIn 重复实验，最快回答最明显质疑。
2. E1 COCO 主结果对齐重测，决定主表是否要换点。
3. E4 最终主表基线协议清理。
4. E3 MS MARCO SafeIn 确认。
5. E5 SafeIn 适用边界压力实验，仅时间充足时做。

