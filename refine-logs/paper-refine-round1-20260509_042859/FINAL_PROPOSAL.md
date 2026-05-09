# Round 1 Refine Proposal

**日期**: 2026-05-09  
**最终判定**: REVISE  
**主线名称**: 面向量化向量检索的候选访问判定  

## Final Method Thesis

本文应被表述为：在基于量化初筛的 ANNS 查询中，量化距离估计及其校准误差界不仅用于近似打分，还可用于决定候选是否进入原始向量重排和原始数据读取路径；`VecFetch` 将候选划分为 `SafeOut`、`Uncertain` 和 `SafeIn`，分别对应保守排除、原始向量验证和可控整记录预取，从而降低 matched-quality 端到端延迟。

这一定义比当前第一版更窄，也更准确。它不声称解决所有端到端向量搜索问题，而是明确依赖“量化表示已经存在、重排/原始记录访问是主要开销”的系统路径。

## Dominant Contribution

**主贡献**: 把量化误差界从“候选打分质量控制”推进到“候选访问路径控制”，用同一个量化初筛结果同时服务于候选剪枝、原始向量验证和原始数据读取时机选择。

当前最强证据应围绕两条机制展开：

1. `SafeOut` 减少进入原始向量读取和重排的候选数量。
2. `Uncertain` 延迟原始数据读取，避免未入榜候选提前读取整条记录，降低尾延迟。

`SafeIn` 应作为机制完整性与未来扩展点，而不是当前 top-10 实验的主要收益来源。

## Complexity Intentionally Rejected

下一轮不要扩大为以下方向：

- 不做图索引主线，不补 HNSW/DiskANN 全量对比。
- 不把论文改成 FlatStor/Lance 通用后端优劣比较。
- 不新增学习式调度、GPU、多线程吞吐或并发服务实验。
- 不把 SafeIn 调成新核心机制，除非补充实验显示其在更大 top-k 或更大 payload 下稳定有益。

## Claims After Refinement

| Claim | 状态 | 需要的证据 | 写法边界 |
|---|---|---|---|
| C1: 在量化初筛路径中，误差界可用于候选访问判定 | Supported as method | 第 3-5 章模型、方法、实现映射 | 摘要和绪论必须加“量化”限定。 |
| C2: `VecFetch` 在相近 recall 下降低端到端延迟 | Partially supported; needs alignment cleanup | COCO/MS MARCO 主表；COCO 需补选点或统一协议重测 | 加速数字必须对应清晰 matched-quality 规则。 |
| C3: `SafeOut` 是当前主要收益来源之一 | Strong on COCO | COCO SafeOut-off；可选 MS MARCO SafeOut-off | 不暗示 MS MARCO 已有完整 SafeOut-off。 |
| C4: `Uncertain` 延迟原始数据读取降低尾延迟 | Supported | COCO/MS MARCO Uncertain-eager | 可写成两个数据集均支持。 |
| C5: `SafeIn` 是可控预取路径 | Limited | 多次重复稳定性；可选 top-k/payload stress | 不能写成当前主收益。 |
| C6: 异步提交策略是次级优化 | Supported as secondary | submit-batch/online 消融 | 放在结果分析或讨论，不放摘要主句。 |

## Planning Gate

- **最终方法 thesis 是否清晰**: 是。必须限定到量化初筛和误差界驱动的访问判定。
- **主贡献是否唯一**: 是。主贡献是候选访问判定，不是新量化算法、图索引或存储格式。
- **刻意拒绝的复杂度**: 图索引实验、全后端矩阵、并发吞吐、SafeIn 强行拔高。
- **仍需验证的 reviewer concern**: COCO 主结果对齐选点；SafeIn-off 单次更快是否为偶然；第 6 章结构是否过重。
- **frontier primitive**: 不涉及 LLM/VLM/Diffusion/RL 等 frontier primitive；不需要相关必要性实验。

## Refine Verdict

当前论文进入 **REVISE** 而不是 READY。文稿主线能修，实验不需要推翻，但第 7 章主表和 SafeIn 消融必须补一轮稳定性证据，否则结论会显得像选择性叙述。

