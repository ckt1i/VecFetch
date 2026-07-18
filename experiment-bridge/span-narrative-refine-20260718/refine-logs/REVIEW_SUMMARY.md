# Review Summary

**Problem**: 将新一轮 span 建模、exact planner 与条件性 SafeIn-aware credit 纳入 RecordGate 论文创新叙述。  
**Initial Approach**: 把 adaptive raw-vector/payload layout 与合并读取数学模型写成两项创新，并考虑以贪心或 exact 求解。  
**Date**: 2026-07-18  
**Rounds**: 2 / 5  
**Final Score**: 9.26 / 10  
**Final Verdict**: READY

## Problem Anchor

- **Bottom-line problem**: 在 record-return 向量检索中，把不可避免的原始向量验证读取转化为受控、可复用的连续 co-fetch，在不改变候选 membership 与 exact verification 语义的前提下，减少小规模随机 I/O 请求；并据此形成一条可被实验直接支撑的论文创新主线。
- **Must-solve bottleneck**: 分离的 vector/payload 读取使系统在候选验证与结果物化阶段产生大量小读；现有 streaming greedy 虽能合并读取，却不保证在固定有序 run 和读放大约束下得到请求数最少、字节数次少的分组，也没有严格利用 span 已覆盖的、可能有用的 inline payload bytes。
- **Non-goals**: 本轮不重新设计 ANN、RaBitQ/SafeOut 或 SafeIn 分类器；不把固定顺序连续分段包装成 NP-hard 装箱；不设计跨查询、跨 tile 的全局布局优化；不声称单次大读在所有设备和负载上总是优于小读；不让 co-fetch 或 SafeIn 改变最终结果正确性。
- **Constraints**: 论文与补实验剩余时间不足两周；复用当前 `.clu` 地址元数据、packed/split layout、64 KiB tile、async/serial reader、现有数据集与结果；优先采用无需训练、可解释、确定性的算法；所有正文结论必须区分已验证的 GE/vector-only exact 与仍需因果消融确认的 SE/SafeIn-aware 扩展。
- **Success condition**: 主方法能被精确定义为一个固定有序 run 上的受约束分段问题；exact planner 与独立二次 oracle 一致并以可接受开销运行；NoSpan、greedy/exact、Combined/NoCombine 的结果分别支撑请求合并、算法最优性和格式—执行协同。SafeIn-aware 只有在 paired 端到端结果、credit/reuse 遥测和 NoCombine 零 credit 边界共同通过时才进入主文，否则降为可选扩展或附录。

## Round-by-Round Resolution Log

| Round | Main Reviewer Concerns | What This Round Simplified | Solved? | Remaining Risk |
|---:|---|---|---|---|
| 1 | GE 被误称 record-aware；exact 过度承担 novelty；SafeIn 证据不足；缺格式×span 因果矩阵 | 主贡献改为格式—执行合同；exact 降为 enabling technique；SafeIn 移出核心；区分 `A_vec/A_eff`；增加两因素矩阵 | yes | 需执行最小因果实验 |
| 2 | 复核贡献焦点、anchor、方法复杂度与实验边界 | 冻结 GE 主线和 SafeIn conditional gate | yes | Vox 非劣边界、interaction telemetry |

## Overall Evolution

- 从“格式创新 + 算法创新 + SafeIn”三条并列，收束为一个 `span-reusable substrate + bounded co-fetch execution` 主贡献。
- 明确 GE 只使用 vector offsets/bytes；格式与 planner 在 completion/materialization 处结合。
- 把主要性能收益归因于 NoSpan→span，把 exact 的价值限定为模型内最优性、确定性和小幅 request improvement。
- SafeIn-aware 从核心创新降为 conditional Pareto extension；eager SafeIn prefetch 从推荐路径删除。
- 加入真实 vector amplification `A_vec` 与 utility-adjusted `A_eff` 的口径隔离。
- 以 `Combined/NoCombine × NoSpan/GE` 取代发散的大型实验矩阵。

## Final Status

- Anchor status: preserved
- Focus status: tight
- Modernity status: appropriately frontier-aware without forced learning components
- Strongest parts: 可执行的格式—读取—复用闭环；严格 exactness scope；已有 NoSpan/GE/NoCombine 证据基础
- Remaining weaknesses: format×span interaction 尚需正式因果表；GE 非劣不能外推 Vox；SafeIn 尚未达到主文门槛
