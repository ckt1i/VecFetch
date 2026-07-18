# Round 1 Review

审稿器：独立 secondary agent（替代当前环境不可用的 Codex GPT-5.4 MCP）  
审稿强度：xHigh-style senior systems/database review  
Reviewer thread：`/root/reviewer`

<details open>
<summary>Full raw reviewer response</summary>

# Round-0 Senior Systems Review

## Verdict

**REVISE**

**Weighted Overall：8.19 / 10**

初稿已经形成了清晰、克制且可实现的系统叙事，但尚未达到 READY。核心问题不是方法不成立，而是：

1. **vector-only GE 并不利用 payload layout，因此“record-aware exact planner”存在耦合过度表述；**
2. **exact 算法的工程增益较小，不能被包装成独立核心创新；**
3. **SafeIn-aware 目前只形成“少请求、多读字节、QPS 小幅回退”的 Pareto 点，证据不足以进入主贡献。**

## 7 维评分

| 维度 | 分数 | 审阅意见 |
|---|---:|---|
| Problem Fidelity | **9.3** | 问题锚点准确：聚焦不可避免的 raw-vector verification I/O，没有漂移到 ANN、NP-hard layout 或学习式调度。正确性边界也很清楚。 |
| Method Specificity | **9.0** | 格式、admission、字典序目标、endpoint-dominance、Fenwick、执行接口和 fallback 均定义充分。需要修正 SE 下“硬读放大合同”的表述。 |
| Contribution Quality | **6.8** | 主张仍有耦合过度风险：GE 是 vector-only planner，格式只让 span 结果可复用，并未参与 GE 的规划决策。exact 算法又是经典前缀 DP 的 dominance 优化，不能单独承担强 novelty。 |
| Frontier Leverage | **8.4** | 使用确定性合同、异步 I/O、size-aware placement 和 exact planning 是自然的现代系统路线；不强行加入 LLM、RL 或学习式 cost model是正确选择。 |
| Feasibility | **8.6** | 核心实现、oracle、43/43 tests 和 CPU 优化都已完成，两周内收口可行。但完整的格式—执行因果证据仍比文中估计更费时间。 |
| Validation Focus | **7.5** | 已有 claim-driven gate 很好，但缺少证明“格式和 span 协同而非简单叠加”的干净 factorial，以及 exact 相对 greedy 的 trace-level practical gap。 |
| Venue Readiness | **6.9** | 写作成熟，但当前版本容易收到三类质疑：“只是 coalescing”“只是经典 DP 优化”“SafeIn 没有带来端到端收益”。需要在贡献措辞和因果实验上进一步收紧。 |

## 三个关键判断

### 1. Exact 是否被过度包装？

**略有过度，但修正成本很低。**

当前提案已经明确“不声称 exact 比 greedy 更快”，这是正确的；但标题、dominant contribution 和 thesis 仍让 exact planner 看起来像新增贡献的主要性能来源。现有结果实际上表明：

- span 的主要收益来自 `NoSpan -> GV`；
- `GV -> GE` 只进一步减少约 `0.22%–1.49%` 请求；
- GE 的 QPS 比 GV 低约 `0.62%–0.89%`；
- endpoint dominance 是固定顺序区间 DP 的经典优化结构。

因此 exact 的正确定位应是：

> 为 bounded co-fetch 提供模型内最优性、确定性和可审计合同的关键实现技术。

不应定位成：

> 一个带来主要端到端性能提升的新优化算法。

### 2. SafeIn 是否应退出主贡献？

**是，当前必须退出。**

`rho=0.1` 的 SE 相对 GE：

- ESCI：请求约 `-0.69%`，QPS约 `-0.40%`；
- MSMARCO：请求约 `-0.22%`，QPS约 `-0.27%`；
- 尚缺 NoCombine zero-credit 因果验证；
- eager SafeIn prefetch 已被结果明确否定。

这足以支持“可选 utility-aware Pareto extension”，不足以支持论文主贡献。建议正文核心方法先完全以 GE 为准；只有 SE 通过 NoCombine、credit-consumption 和多数据集端到端门禁后，才在贡献 2 末尾加入一句扩展。

### 3. 格式与算法能否合成一个贡献？

**可以，但贡献对象必须是完整的格式—执行合同，而不是宣称算法本身格式感知。**

当前 GE 的 admission 只使用 vector offsets 和 vector bytes；它不会因为 payload inline/external 而改变规划。因此更准确的闭环是：

1. 自适应格式使小 payload 位于 mandatory vector span 可覆盖的物理路径中；
2. bounded planner 决定哪些连续 vector extents 值得作为一次读取；
3. completion 把 span 已覆盖的 inline payload 暴露为 reusable view；
4. final materialization 只补读缺失部分。

这可以形成一个系统贡献：

> **A span-reusable record substrate with amplification-bounded exact co-fetch.**

但不要称 GE 为“layout-aware/record-aware optimization”。只有 SE 真正使用 `S(i,j)` 时，planner 才是严格意义上的 record-aware admission。

## 高优先级修订点

1. **P0：修正主贡献的耦合表述。** 将 dominant contribution 改成“span-reusable format 与 bounded co-fetch execution 的联合合同”；把 endpoint-dominance exact planner 降为其中的关键技术。避免直接写“record-aware exact planner”描述 GE。
2. **P0：从主贡献和核心流程中移出 SafeIn。** 核心方法冻结为 GE。SE 单列为 conditional extension；失败时进入 appendix/limitations，不影响主线成立。
3. **P0：区分两种 amplification 语义。** GE 的 `B<=αV` 是相对 mandatory vector bytes 的硬放大上限；SE 的 `B<=α(V+ρS)` 是 utility-adjusted admission，不能继续称为相同的 vector-byte amplification contract。必须同时报告真实 `B/V`。
4. **P0：收缩 exact 的 novelty claim。** 使用“we derive an exact `O(n log n)` planner for our fixed-order admission model”，不要使用“new optimal I/O algorithm”或暗示设备时延最优。明确主要性能收益来自 bounded span，exact 提供最优性与稳定合同。
5. **P1：补一项最小格式—执行因果实验。** 至少形成 `Combined/NoCombine × NoSpan/GE` 的受控比较，并报告 span-resident reusable/consumed bytes 与 final payload requests。否则 Combined 相对 NoCombine 的 QPS差异不能直接证明 co-design synergy。
6. **P1：补 exact 的 practical-necessity 证据。** 报告 greedy 与 exact 产生不同计划的 run 比例、每-query request/byte gap 分布，以及实际 `m<=21` 下 `O(n²)` direct DP 与 Fenwick 的 CPU 对比。若 bounded DP 更快，采用混合实现不会削弱理论结论，反而更符合系统设计。

## 推荐的最终主线

> RecordGate first eliminates avoidable exact verification through bound-guided access control. For the remaining mandatory reads, it uses a size-adaptive, span-reusable record substrate and an amplification-bounded exact co-fetch planner to reduce physical requests and reuse inline payload bytes without changing exact-verification semantics.

这条主线可以成立，而且比“自适应格式 + 新优化算法 + SafeIn”三项并列更集中。SafeIn 最多作为该机制的条件性 utility extension，不应成为当前版本的创新支点。

</details>
