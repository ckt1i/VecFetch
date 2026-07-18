# RecordGate 移除 SafeIn 与重构论文主线：研究评审

日期：2026-07-18  
评审对象：SafeIn 去留、SafeOut 理论边界、span/layout/pipeline 新叙事与两周内补实验范围

## 1. 执行结论

**建议将 SafeIn 从论文方法、贡献、默认运行配置和主实验中移除。** 这会增强而不是削弱当前论文：最新严格实验不支持 SafeIn 形成稳定收益，它的 early positive classification 又不是跨扫描单调的最终 top-k membership 证书；继续保留会引入大量 credit、tail、confidence 与 wasted-prefetch 概念，掩盖已经成立的 SafeOut、bounded span 和 natural payload reuse。

需要区分三种移除：

| 层面 | 决策 | 原因 |
|---|---|---|
| 论文主线 | 立即移除 | 与最新严格证据对齐，减少 heuristic stacking |
| 默认执行路径 | 立即关闭 | eager/cold/SE/tail 均未显示稳定收益；冻结可审计配置 |
| 物理删除代码 | 投稿后再做 | 两周内删除成熟路径的回归风险高于论文收益；先保留非默认诊断 flag |

建议保留的 SafeIn 内容只有 appendix/design alternatives 中的一小段负结果：说明其理论上是 speculative、实验上无效字节比例高且吞吐不稳定，因此 RecordGate 选择 one-sided confidence control。

## 2. 新论文主线是否成立

**成立，但应从用户提出的三条松散创新，重构为两条耦合的核心机制。**

### 核心机制一：从候选证据到 mandatory verification I/O

RecordGate 使用 RaBitQ 概率距离区间和不断收紧的 conservative top-k upper frontier，对候选执行 one-sided exclusion。满足 lower-bound exclusion 条件的候选无需读取 raw vector；当前未被排除的候选进入 admitted verification read set。

这里的贡献不是“动态阈值”或“SafeIn/SafeOut 双向分类”，而是一个有明确置信合同、能直接控制 record I/O 的 candidate-to-record access controller。

### 核心机制二：从 admitted vector I/O 到 record materialization reuse

RecordGate 在固定 tile、地址有序的候选 run 内，把读取合并构造为带读放大约束的一维连续区间分段；利用 endpoint dominance 在 `O(n log n)` 内求解冻结模型的 exact partition，并在 IVF probing 期间异步发起这些 reads。

size-adaptive record substrate 将小 payload 或 payload prefix 与 raw vector 共置。只有当当前 admitted vector span 已自然覆盖这些 payload bytes、extent 校验通过且 final materializer 实际消费时，系统才把它计为 reusable view；candidate-set top-k sealed 后再补读缺失 payload。它是 **natural cofetch/reuse**，不是基于 membership prediction 的 speculative prefetch。

### 一句话故事

> RecordGate turns bounded candidate evidence into a mandatory verification-read set, executes that set as amplification-bounded spans over reusable records, and reuses any inline payload bytes already covered by those reads.

## 3. 必须修正的理论表述

### 3.1 SafeOut 的单调性

设已见候选为 `C_t`，真实距离为 `d_i`，概率区间为 `[L_i,U_i]`，`T_t` 是已见候选中第 k 小的 upper bound。若至少已有 k 个候选且

\[
L_i>T_t,
\]

则在联合区间事件

\[
\mathcal E=\bigcap_j\{L_j\le d_j\le U_j\}
\]

成立时，已有至少 k 个候选严格优于 `i`。随着候选加入，第 k 小 upper bound 不会上升，所以该 exclusion certificate 只会增强，不会撤销。

论文必须同时声明：

1. 这是相对于 ANN/IVF 已生成候选集的 post-ANN verification 结论，不补偿 candidate generation 的漏召；
2. 正确性条件是 query-level 联合 bound event，而不是把单候选 marginal coverage 直接当作整次查询的 coverage；
3. 使用严格不等式并定义 distance tie policy；
4. 自适应扫描所使用的 bound 也必须包含在 simultaneous/union-budget 口径中。

推荐表述：

> Conditional on the configured query-level bound event, once a candidate is excluded by the lower-bound test, the evolving frontier can only make that certificate stronger.

不要写“SafeOut 无误判”“保证全库 exact top-k”或“单候选 `1-delta` 自动等于 query-level `1-delta`”。

### 3.2 SafeOut 不等于所有早发 reads 都是必要的

一个当前 Uncertain 的候选可能在之后随 frontier 收紧而变成 SafeOut。如果系统已经提前发出它的 raw-vector read，这部分是为了隐藏延迟而付出的额外 work，不是 accuracy error。

因此应区分：

- SafeOut certificate 的单调性；
- Uncertain 状态的暂时性；
- early issuance 的 latency--work tradeoff。

若能低成本增加 counters，应统计“发出后最终变成 SafeOut”的候选数与字节；否则正文只写 `currently admitted verification reads`，不写所有提前读取均为最终必要读取。

### 3.3 SafeIn 为什么应移除

SafeIn 使用当前已见集合的 lower/reference frontier 时，只能说明候选在当前集合中很强。未来出现更好候选后，早期 membership 判断可以失效。要形成最终候选集 top-k 证书，必须等待候选扫描完成，届时已经失去 early payload I/O 的重叠机会。

正确结论不是“SafeIn 理论上必然失败”，而是：

> SafeIn lacks a scan-monotone final-membership certificate and therefore remains speculative. The controlled experiments show that its precision and hidden-I/O benefit are insufficient on the evaluated workloads.

理论说明它为什么风险更高，实验决定为什么当前系统应删除它。

## 4. 对原三点叙事的评审

### 原第 1 点：固定 rerank 阈值

“传统方案缺乏理论依据，而且误差大”目前不成立。固定 budget 可以通过 validation recall 或 cost tradeoff 选择；budget 足够大时不一定误差大，只是读取更多。现有 NoSafeOut 是 verify-all 上界，不是传统 fixed-depth baseline。

安全改写：

> Static rerank budgets apply the same verification effort despite query- and candidate-level variation in estimator uncertainty and frontier evolution. RecordGate instead makes candidate-level verification decisions from bounded distance evidence and an evolving top-k frontier.

只有补完 matched-recall fixed-R curve 后，才能进一步写“在同 recall 下减少 verification I/O”。SafeIn 移除后，此处不再出现 payload prefetch。

### 原第 2 点：pipeline + DP + exact planner

方向成立，但应合并为一个 physical execution contribution 的三个层次：

1. **when**：在 probing 期间异步提交当前已接纳的 verification reads；
2. **what**：把零散读取合并为受 amplification 约束的 spans；
3. **how**：在固定顺序、同 tile、固定 admission 和冻结目标下 exact 求解分段。

`O(n log n)` 不是一般 I/O planning 的最优算法，也不应声称比 greedy 吞吐更高。当前 exact 相对 greedy 进一步减少少量 requests，但 QPS 略有回退。其价值是模型内可审计最优性与低规划成本；span 本身才是主要性能来源。

现有 Full/NewNoPipeline 同时改变 batching、reuse、tail、SafeIn 和同步 final path，不能把全部差距归因于 CPU/I/O overlap。需要公平的 `NoOverlapAsyncFinal`；否则把 pipeline 降为执行机制而非独立性能贡献。

### 原第 3 点：自适应 record layout

单独的“小 payload inline、大 payload external”容易被视为已有 inline/out-of-line placement 与 late materialization 的组合。新颖性应放在完整执行合同：

`bound evidence -> mandatory verification set -> bounded spans -> completion-time payload views -> missing-payload fetch`

论文还需说明 prefix/suffix locator、是否复制、更新与一致性、view lifetime、部分覆盖 completion、padding/descriptor 字节计费，以及 final materializer 如何判定缺失区间。

因此 format 不应成为孤立的第三条 headline contribution，而应是 span-reusable physical execution 的 substrate。

## 5. 证据评审

### 已足够复用

- **SafeOut vs NoSafeOut**：可证明 pruning 避免 verify-all 的巨大成本；不能证明优于 fixed budget。
- **NoSpan vs bounded span**：可证明 span 显著减少 requests，并在 ESCI/MSMARCO 提高 QPS。
- **Exact vs greedy + oracle/tests**：可证明冻结模型内 exactness、少量 request improvement 与实际 planner overhead；不能声称 exact 提高吞吐。
- **Eager/cold/SE SafeIn**：足以支持 default-off 和 rejected-design appendix，无需再 sweep。
- **Natural reuse**：可证明在 ESCI/MSMARCO、且 mandatory span 覆盖 inline bytes 时减少 final materialization I/O；不能声称四数据集普遍 QPS 加速。

### 不能直接复用为强因果结论

- Full vs NewNoPipeline 不能隔离 cross-probe overlap；
- Combined/NoCombine 历史结果混合 planner、reuse、cache/layout 等因素；
- 旧 P0/P1 个别 SafeIn 正点不能覆盖最新严格 negative evidence；
- NoSafeOut 不能替代 static fixed-depth rerank baseline。

完整矩阵见 [CLAIMS_MATRIX_CN.md](./CLAIMS_MATRIX_CN.md)，最小补实验见 [MINIMAL_EXPERIMENT_PLAN_CN.md](./MINIMAL_EXPERIMENT_PLAN_CN.md)。

## 6. 相关工作与新颖性风险

[FusionANNS (FAST '25)](https://www.usenix.org/conference/fast25/presentation/tian-bing) 已经包含 heuristic reranking、redundant-aware I/O deduplication 和 optimized layout；[VeloANN](https://arxiv.org/abs/2602.22805) 和 [Starling](https://arxiv.org/abs/2401.02116) 也分别强调 page/block locality、layout 与异步或 I/O-efficient execution。因此不能把“合并小读”“把数据放在一起”或“异步 I/O”单独包装成首创。

RecordGate 应强调跨边界闭环：候选特定的 bound evidence 先决定 mandatory read set，exact bounded-span planner 再执行该集合，最后把其已经覆盖的 inline payload 转化为 materialization reuse。实验也必须逐层证明这些连接，而不是只给 full-stack 数字。

## 7. 预期审稿判断

第一轮按原三点叙事和当前证据，独立审稿人的模拟评分约为 **5/10（Weak Reject/Borderline）**；完成 SafeIn paper removal/default-off 与两条式改写后，预计约为 **6/10（Borderline）**。主要缺口不是 SafeIn，而是：

1. 尚无 fixed rerank matched-recall baseline；
2. pipeline overlap 未被公平隔离；
3. layout--span interaction 证据仍弱；
4. query-level probability accounting 尚未冻结。

若三个 P0 通过并收紧 correctness contract，预期可提升到约 **7/10（Weak Accept）**。继续调 SafeIn 不会显著提高接受概率。

## 8. 最终建议

1. 立即冻结 `SafeIn-free` 论文配置：credit/eager/cold-prefix/tail 全关，natural span reuse 保留。
2. 用两条主机制重写 abstract/overview：`bound-guided verification` 与 `span-reusable physical execution`。
3. 把 exact `O(n log n)` 定位为模型内 exact enabling technique，把 bounded span 定位为主要性能机制。
4. P0 优先补 fixed-R、`Combined/NoCombine × NoSpan/GE` 和 SafeIn-free correctness/config audit；NoOverlapAsyncFinal 降为 P1，除非坚持 headline overlap claim。
5. 冻结 candidate-set scope、joint confidence event、strict inequality/tie policy 和 early-issued waste 口径。
