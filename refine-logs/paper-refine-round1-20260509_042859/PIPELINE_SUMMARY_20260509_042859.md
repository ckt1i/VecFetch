# Pipeline Summary

**Problem**: 第一版外部批注指出论文把问题写得过泛，且 COCO 主实验选点和 SafeIn 消融稳定性仍可能被质疑。  
**Final Method Thesis**: `VecFetch` 应被定义为量化初筛路径中的候选访问判定机制，用量化估计和校准误差界控制原始向量重排与原始数据读取时机。  
**Final Verdict**: REVISE  
**Date**: 2026-05-09

## Final Deliverables

- Proposal: `FINAL_PROPOSAL.md`
- Review summary: `REVIEW_SUMMARY.md`
- Refinement report: `REFINEMENT_REPORT.md`
- Experiment plan: `EXPERIMENT_PLAN.md`
- Experiment tracker: `EXPERIMENT_TRACKER.md`

## Contribution Snapshot

- **Dominant contribution**: 量化误差界驱动的候选访问判定，把候选分为 `SafeOut`、`Uncertain`、`SafeIn` 并映射到不同原始记录读取动作。
- **Supporting contribution**: 异步 I/O 提交策略帮助必要读取与探测/重排重叠，但只是次级优化。
- **Explicitly rejected complexity**: 图索引主线、通用后端比较、并发吞吐、学习式调度、强行拔高 SafeIn。

## Must-Prove Claims

1. 在量化初筛路径中，`VecFetch` 能在 matched-quality 口径下降低端到端延迟。
2. `SafeOut` 与 `Uncertain` 是当前实验中证据最强的访问判定机制。
3. `SafeIn` 在当前 top-10 下触发少，不能写成主收益；若要写收益，必须通过重复或压力实验支持。

## First Runs to Launch

1. COCO Full vs SafeIn-off repeat, top10, nprobe=64, 5-10 次。
2. COCO 主结果 matched-quality 对齐重测，VecFetch 与 IVF+RaBitQ FlatStor 在相邻 nprobe 点上补跑。
3. 最终主表基线点的 warmup + measurement 协议清理。

## Main Risks

- **Risk**: 严格对齐后 COCO 加速变小。  
  **Mitigation**: 使用 Pareto 曲线和共同阈值规则解释；把主叙事权重转向 MS MARCO 与机制消融。
- **Risk**: SafeIn-off 稳定更快。  
  **Mitigation**: 正文承认 SafeIn 当前不是收益来源，只保留为可控预取路径和未来工作。
- **Risk**: 第 6 章移附录后复现信息不足。  
  **Mitigation**: 主文保留公开参数表；附录保留公开可复现口径，不保留本机私有路径。

## Next Action

先执行 `R1-E2` 和 `R1-E1`，再根据新结果改第 7 章主表、SafeIn 小节、摘要和结论。

