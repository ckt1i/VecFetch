# Refinement Report

**Problem**: 为 RecordGate 最新 span/format/exact/SafeIn-aware 方案冻结论文创新叙述。  
**Initial Approach**: 自适应 raw-vector+payload 格式与合并读取数学模型作为两项创新。  
**Date**: 2026-07-18  
**Rounds**: 2 / 5  
**Final Score**: 9.26 / 10  
**Final Verdict**: READY

## Problem Anchor

完整锚点见 `PROBLEM_ANCHOR.md`，并已逐字保留在每轮 proposal/refinement 中。

## Output Files

- Clean final proposal: `refine-logs/FINAL_PROPOSAL.md`
- Review summary: `refine-logs/REVIEW_SUMMARY.md`
- Initial proposal: `refine-logs/round-0-initial-proposal.md`
- Round 1 raw review: `refine-logs/round-1-review.md`
- Full Round 1 refinement: `refine-logs/round-1-refinement.md`
- Round 2 raw review: `refine-logs/round-2-review.md`
- Score history: `refine-logs/score-history.md`

## Score Evolution

| Round | Problem Fidelity | Method Specificity | Contribution Quality | Frontier Leverage | Feasibility | Validation Focus | Venue Readiness | Overall | Verdict |
|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 1 | 9.3 | 9.0 | 6.8 | 8.4 | 8.6 | 7.5 | 6.9 | 8.19 | REVISE |
| 2 | 9.6 | 9.5 | 9.1 | 9.0 | 9.2 | 9.2 | 8.8 | 9.26 | READY |

## Round-by-Round Review Record

| Round | Main Reviewer Concerns | What Was Changed | Result |
|---:|---|---|---|
| 1 | planner-level coupling 虚高、classic DP overpackaging、SafeIn evidence gap | 改为 execution-level co-design；exact 降级；GE 冻结；SafeIn conditional；补 amplification 与 factorial contract | resolved |
| 2 | 检查 anchor、贡献聚焦、复杂度和实验可执行性 | 无 blocking change；记录五项 non-blocking handoff | READY |

## Final Proposal Snapshot

- Paper-level logic: SafeOut 决定“是否读”，bounded co-fetch 决定“不可避免的 reads 如何组织”。
- New physical contribution: `span-reusable record substrate with amplification-bounded exact co-fetch`。
- Exact planner: fixed ordered run 上 `(requests,bytes)` 字典序最优的 `O(n log n)` enabling technique。
- SafeIn: 只允许作为 conditional utility credit；eager prefetch 默认关闭。
- Minimal validation: NoSpan→GV→GE；`Combined/NoCombine × NoSpan/GE`；可选 SE zero-credit/consumption gate。

## Method Evolution Highlights

1. 把两项松散创新合并成一个格式—执行合同。
2. 把 exact 从“性能新算法”校正为“模型内最优性的系统技术”。
3. 删除独立 eager SafeIn 路径，保证主方法线性、可解释且两周内可冻结。

## Pushback / Drift Log

| Round | Potential Drift | Author Response | Outcome |
|---:|---|---|---|
| 1 | 用 workload-aware global placement 提升 novelty | 拒绝；改变问题且超时 | retained as future work only |
| 1 | 加 learned latency/cost model | 拒绝；当前 bottleneck 有确定性一维结构 | excluded |
| 1 | 把 exact 单列算法创新 | 收缩；经典 DP optimization 不单独主张 novelty | merged into main mechanism |

## Remaining Weaknesses

- `Combined/NoCombine × NoSpan/GE` interaction 尚未正式完成；当前 GE NoCombine 点只能做 drift anchor。
- exact 相对 greedy 的 different-plan ratio 与 gap distribution 尚需补 telemetry。
- GE 的 empirical non-inferiority 只覆盖 ESCI/MSMARCO 核心点，Vox 是反例边界。
- SE/`rho=0.1` 仍未通过 NoCombine zero-credit 与 consumed-credit gate。

## Raw Reviewer Responses

完整原文分别保存在 `round-1-review.md` 与 `round-2-review.md`，未在本文件重复复制，以避免日志漂移。

## Next Steps

1. 把 `FINAL_PROPOSAL.md` 的 core paragraph、contribution bullets 与 anti-claims 同步到 `NARRATIVE_REPORT.md`、Method Contract、Introduction/System Design/Experiments claim ledger。
2. 运行最小格式×span factorial 与 exact plan-gap telemetry。
3. SafeIn 仅运行一次 conditional gate；失败即固定 GE 并停止方法扩张。
