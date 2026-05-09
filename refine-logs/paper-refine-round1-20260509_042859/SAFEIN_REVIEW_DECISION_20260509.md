# SafeIn 是否保留的研究评审结论

**日期**: 2026-05-09  
**Reviewer route**: Codex subagent, `gpt-5.4`, xhigh reasoning  
**Agent id**: `019e0cbc-89cb-7f13-8df9-a45442bdb4e0`  
**问题**: SafeIn 重复实验显示开启/关闭性能差异很小，是否应从论文中完全移除？

## 输入证据

### COCO100K SafeIn repeat

- Full: median avg `1.1981 ms`, median p99 `1.6898 ms`, SafeIn prefetch `0.109/query`, final fetch `9.913/query`
- SafeIn-off: median avg `1.1985 ms`, median p99 `1.7031 ms`, SafeIn prefetch `0/query`, final fetch `10/query`
- 判定: 差异在噪声内，SafeIn 当前 top-10 下不显著。

### MS MARCO SafeIn repeat

- Full: median avg `10.2861 ms`, median p99 `18.5116 ms`, SafeIn prefetch `0.322/query`
- SafeIn-off: 稳定子集接近 Full，但存在明显 I/O tail outlier
- 判定: 不能支持 SafeIn 稳定收益，也不能支持 SafeIn 稳定有害；更合理的说法是当前 top-10 下不显著且受尾延迟噪声影响。

### 其他机制证据

- SafeOut: COCO SafeOut-off 后 reranked candidates 从约 `232/query` 增至 `3149/query`，avg 从 `1.2818 ms` 增至 `7.4248 ms`，p99 从 `1.8135 ms` 增至 `9.4914 ms`。
- Uncertain delayed original-data fetch: Uncertain-eager 后 COCO p99 增约 `15.5%`，MS MARCO p99 增约 `28%`，recall 不变。

## 外部 reviewer 结论

Reviewer 的核心判断：

> SafeIn 作为并列核心设计，应移出主线；SafeIn 作为可选实现细节，可以保留。继续把它当主贡献写，会伤论文。

Reviewer 认为：

- 不应把 SafeIn 保留为主贡献。
- 不建议从全文彻底删除 SafeIn，因为实现和访问路径中确实存在该可选分支。
- 应将论文主线改为“两项被证据支持的核心机制 + 一个可选预取 heuristic”：
  - 核心机制 1: SafeOut 保守排除。
  - 核心机制 2: Uncertain 原始向量验证 + 延迟原始数据读取。
  - 可选机制: SafeIn speculative / opportunistic / implementation-level full-record prefetch path。

## 最终采纳方案

采用 **“保留但降级”**，不采用“完全移除”。

### 保留的位置

- 第 3 章模型中可保留 SafeIn 作为候选状态的可选输出，但应避免把它写成与 SafeOut/Uncertain 同等被实验验证的主机制。
- 第 4 章方法中保留 SafeIn 的条件和访问动作，但改写成 optional / opportunistic / bounded prefetch path。
- 第 5 章实现中保留 `CandidateClass`、`VEC_ALL` 和 SafeIn 相关代码映射，因为这是实现事实。
- 第 8 章讨论中保留 SafeIn 的负结果/边界分析。
- 附录可保留 SafeIn-off 重复结果表。

### 移除或降级的位置

- 摘要中删除 SafeIn 性能收益表述。
- 绪论贡献列表不把 SafeIn 作为核心贡献。
- 主结果 claim 不写 SafeIn。
- 第 7 章机制分析主表/主图优先展示 SafeOut-off 与 Uncertain-eager。
- SafeIn-off 结果从主 headline 表格移到讨论或附录。

## 允许写的 claim

- VecFetch 的主要收益来自 SafeOut 的保守排除，以及 Uncertain 候选的原始向量验证与延迟原始数据读取。
- SafeIn 在当前 top-10 工作负载中触发很少。
- 当前实验未显示 SafeIn 对平均延迟或尾延迟有一致、稳定的改善。
- SafeIn 是实现中的可选整记录预取路径，不是本文主结论的必要组成部分。

## 不允许写的 claim

- VecFetch 的三分类访问策略已经被完整实验证明。
- SafeIn 能稳定降低 avg latency 或 p99 latency。
- SafeIn 是端到端收益的重要来源。
- SafeIn 至少无害。
- 当前结果支持将 SafeIn 作为通用有效机制推广。

## 最小论文修改方案

1. 摘要、绪论贡献列表、结论主句中删除 SafeIn 主张。
2. 第 3/4/5 章把核心框架改为 SafeOut + Uncertain；SafeIn 改成 optional prefetch branch。
3. 图中如果出现 SafeIn，使用虚线或灰色表示 optional branch。
4. 第 7 章主机制消融只重点讲 SafeOut-off 和 Uncertain-eager。
5. SafeIn-off 重复表放附录，正文仅保留负结果/不显著结论。
6. 第 8 章加强边界表述：SafeIn 当前 top-10 下不是论文主结论支柱。

