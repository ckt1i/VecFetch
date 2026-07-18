# Round 2 Review

审稿器：与 Round 1 相同的 independent secondary agent  
Reviewer thread：`/root/reviewer`

<details open>
<summary>Full raw reviewer response</summary>

# Round-2 Re-evaluation

## Verdict

**READY**

**Weighted Overall：9.26 / 10**

修订稿已经解决首轮的三个核心问题：GE 与格式的关系不再被虚构为 planner-level coupling；exact 被降为保证系统合同可证明的 enabling technique；SafeIn 退出核心方法。当前提案可以作为后续论文叙事和最小补实验的冻结版本。

## 7 维评分

| 维度 | 分数 | 加权 |
|---|---:|---:|
| Problem Fidelity | 9.6 | 15% |
| Method Specificity | 9.5 | 25% |
| Contribution Quality | 9.1 | 25% |
| Frontier Leverage | 9.0 | 15% |
| Feasibility | 9.2 | 10% |
| Validation Focus | 9.2 | 5% |
| Venue Readiness | 8.8 | 5% |
| **Overall** | **9.26** | |

## 核心复审结论

### Problem Anchor：Preserved

Problem Anchor 完整保留，没有因 reviewer 建议漂移到：

- workload-aware record reordering；
- 跨查询或跨 tile 全局规划；
- learned latency model；
- 新 SafeIn 分类器；
- NP-hard layout optimization。

新方法仍直接处理原始 bottleneck：把不可避免的 candidate-local vector reads 组织为有硬放大边界、可复用 payload bytes 的连续读取。

### Dominant contribution：明显更聚焦

主贡献现在是：

> span-reusable record substrate + amplification-bounded co-fetch execution contract

这是合理的单一系统贡献。格式、planner、completion view 和 final missing fetch 分别回答：

1. 哪些 payload bytes 可能位于 mandatory path；
2. 哪些连续 vector extents 可以读取；
3. 已读取 bytes 如何变成合法 view；
4. 哪些 payload 仍需补读。

GE 不再被错误描述为 record-aware optimizer。格式与 planner 的结合点被准确放在 completion/materialization，而不是算法输入层。

### Exact：不再过度包装

修订稿正确地区分了：

- `NoSpan -> span`：主要端到端收益；
- `GV -> GE`：消除 heuristic gap、提供模型内最优性和少量额外请求减少。

`O(n log n)` endpoint-dominance 被定位为针对系统合同推导出的 exact implementation，而不是一般性算法突破。这一包装强度合适。

### SafeIn：退出核心方法是正确决定

GE 已被冻结为默认主线。SE 仅作为 conditional Pareto extension，并明确：

- `A_vec` 与 `A_eff` 不是同一口径；
- credit 不减少物理字节；
- credit 不保证 payload 最终被消费；
- NoCombine 必须为零 credit；
- eager SafeIn prefetch 默认关闭。

因此，即使后续 SE gate 失败，也不会破坏主方法、贡献列表或实验结构。

### Method simplicity：显著改善

方法已收缩为一条线性执行路径：

```text
logical pruning
→ mandatory vector runs
→ bounded exact partition
→ span completion
→ vector verification + inline payload views
→ final missing-payload fetch
```

没有平行的 eager prefetch path，也没有加入不必要的学习器、auto-tuner 或跨查询状态。复杂度与两周时间约束匹配。

### Frontier leverage：充分且自然

方案正确使用了现代存储系统原语：

- direct-address resident metadata；
- size-adaptive inline/external placement；
- contiguous asynchronous span reads；
- completion-time zero-copy/query-local views；
- deterministic exact planning；
- explicit byte-amplification contract。

不引入 LLM、RL 或 learned cost model是正确选择，而非技术落后。

## Remaining non-blocking action items

1. 完成 `Combined/NoCombine × NoSpan/GE` 最小两因素实验，重点报告 interaction、reusable/consumed bytes 和 final payload requests，而不只报告 QPS。
2. 增加 exact plan-gap telemetry：greedy/exact 不同计划比例、request/byte gap 分布和实际 run-size 分布。可顺手比较实际 `m<=21` 下 direct DP 与 Fenwick，但不必扩展成新的算法章节。
3. 论文中不要把 GE 称为全数据集“empirically non-inferior”。ESCI/MSMARCO 支持该结论，Vox 当前不支持；Vox 应作为 CPU-sensitive boundary 或 GV fallback 案例。
4. SafeIn 只有通过 Combined/NoCombine credit causality 和 consumed-credit gate 后，才允许在 contribution 2 末尾增加一句。否则保留在附录或 limitations。
5. Related Work 最终增加一张紧凑对比表，明确 FusionANNS、VeloANN、Starling、Lance/Parallax 与本方法在 layout、online planning、hard amplification、payload reuse 和 exact partition 上的边界。

当前不存在阻止冻结 proposal 的 blocking issue。实验尚待完成属于正常的 validation handoff，而不是方法设计缺陷。

</details>
