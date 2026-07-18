# RecordGate SafeIn 移除研究评审：文档索引

日期：2026-07-18

## 最终结论入口

- [RESEARCH_REVIEW_SAFEIN_REMOVAL_CN.md](./RESEARCH_REVIEW_SAFEIN_REMOVAL_CN.md)：完整研究评审、理论边界、证据判断、两条新主线与最终决策。
- [PAPER_NARRATIVE_REWRITE_CN.md](./PAPER_NARRATIVE_REWRITE_CN.md)：可直接用于 Abstract/Introduction 的两条贡献表述、原三点逐项改写和章节结构。
- [CLAIMS_MATRIX_CN.md](./CLAIMS_MATRIX_CN.md)：每条候选主张的证据、可写范围、缺口和禁止表述。
- [MINIMAL_EXPERIMENT_PLAN_CN.md](./MINIMAL_EXPERIMENT_PLAN_CN.md)：少于两周时限下的三个 P0、可复用结果、P1 与日程。
- [PROJECT_MEMORY_UPDATE_CN.md](./PROJECT_MEMORY_UPDATE_CN.md)：本轮冻结决策、术语和后续工作清单。

## 评审过程与原始记录

- [REVIEW_CONTEXT_CN.md](./REVIEW_CONTEXT_CN.md)：发送给独立 reviewer 的自包含代码、实验与理论上下文。
- [round-1-review.md](./review-logs/round-1-review.md)：第一轮 ICDE/PVLDB 风格完整原始评审、mock review 和优先级。
- [round-2-review.md](./review-logs/round-2-review.md)：第二轮收敛评审、三个 P0、可直接采用的英文段落和最终 verdict。

本轮因环境中没有可调用的 Codex review MCP，使用同一研究上下文下的独立 xHigh secondary reviewer 完成两轮评审；原始输出未压缩保存，主线程仅在最终文档中综合和修正。

## 冻结 Verdict

| 决策 | 结论 |
|---|---|
| SafeIn 从 paper 主方法、贡献、主图与主结果移除 | YES |
| SafeIn 在论文默认执行路径关闭 | YES |
| 两周内物理删除全部 SafeIn 代码 | NO |
| 两条式新论文叙事 | REVISE, THEN ACCEPT |

三个 P0：

1. Fixed-R matched-recall frontier；
2. `Combined/NoCombine × NoSpan/GE` layout--span interaction；
3. SafeIn-free correctness/configuration audit。

`NoOverlapAsyncFinal` 为 P1；只有保留 headline overlap claim 时升级为 P0。

