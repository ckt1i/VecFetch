# Round 2 收敛评审：SafeIn 移除后的最终论文方向

日期：2026-07-18  
评审口径：ICDE/PVLDB/系统论文；剩余执行时间少于两周。

## 1. 收敛结论与评分变化

若论文正式采用以下两条核心贡献：

1. **One-sided bound-guided verification**：只使用具有扫描单调性的排除证书控制 raw-vector verification；
2. **Span-reusable physical execution**：对当前 admitted verification reads 做受读放大约束的物理合并，并复用 mandatory spans 已自然覆盖的 inline payload bytes；

同时从 paper/default path 完全移除 SafeIn，我认为方向是正确的，叙事质量会明显提高。它把原来的“SafeOut + SafeIn + eager prefetch + prefix + tail + span credit”启发式堆叠，收束为一个 one-sided logical controller 和一个 correctness-neutral physical executor。

评分需要区分“只改叙事”和“补完证据”：

- **仅完成 SafeIn paper removal/default-off 与两条贡献重写**：由 Round 1 的 `5/10` 提升到约 **6/10，Borderline**。结构更清楚，但 fixed-R 对照、layout–span interaction 和 confidence contract 仍缺失。
- **下述三个 P0 全部完成且结果通过**：可提升到约 **7/10，Weak Accept**，confidence `4/5`。
- **若 fixed-R 未显示 matched-recall 优势**：第一条贡献仍可保留为 adaptive bounded verification，但必须删除相对 static rerank 的优越性表述，整体上限约 `6/10`。
- **若 layout×span interaction 未成立**：第二条会退化成 I/O coalescing + 常规 inline layout 的组合，novelty 风险很高，整体约 `5--6/10`。
- **NoOverlap 未补或未通过**不会直接否定两条核心贡献，只要求删除“跨-probe overlap 带来独立加速”的表述。因此它不是本轮最优先实验。

## 2. 两周内压缩后的三个 P0

### P0-1：Fixed-R matched-recall frontier

这是第一条贡献最关键的外部 baseline，而 NoSafeOut 只能代表 verify-all 上界。

- 数据集：ESCI、MSMARCO；不扩展大矩阵。
- 固定：相同 IVF candidates/nprobe、候选顺序、span/layout/io_uring/final materialization、cache conditioning。
- 比较：RecordGate one-sided controller vs `3--5` 个 fixed-R 点。
- 报告：Recall@k、raw-vector requests/bytes、reranked count 分布、QPS、p99；做 matched-recall 或完整 recall–work Pareto，而不是只选一个有利 R。
- 通过条件：RecordGate 在不低于 baseline recall 置信区间时稳定减少 verification work，或形成严格更优 Pareto frontier。

### P0-2：`Combined/NoCombine × NoSpan/GE` 两因素 interaction

这比 NoOverlap 更优先，因为它直接决定第二条 contribution 能否被称为“span-reusable record substrate”，而不只是普通 read coalescing。

- 数据集：优先 ESCI、MSMARCO，复用既有索引和配置。
- 四个 cell 必须保持相同 candidate set、SafeOut、reader、cache 和 top-k；SafeIn 全关。
- 报告：vector/payload requests、physical bytes、vector-byte amplification、eligible/covered/registered/actually-consumed inline bytes、final missing-payload requests/bytes、QPS/p99。
- 核心不是比较 Combined 与 NoCombine 的绝对 QPS，而是检验 interaction：GE 在两种 layout 下均可减少 vector requests，但只有 Combined 应产生合法 payload views，并相对自身 NoSpan 减少 final materialization work。
- 若 `NoPayloadReuse` 已能在相同 Combined layout 中关闭 view consumption，可作为补充机制 cell，但不能代替 NoCombine 的 zero-eligibility 边界。

### P0-3：SafeIn-free correctness/configuration audit

该项不是大实验，而是论文与 artifact 的联合冻结审计：

1. 默认配置证明 `eager SafeIn=off`、`cold-prefix=off`、`SE credit=0`、`tail=0`，但 natural span reuse 仍开启；
2. 明确 guarantee 只相对 ANN/IVF candidate set，并把 candidate-generation recall 单列；
3. 给出 query-level joint bound event 或 failure-budget accounting；若当前只有 marginal calibration，则把 guarantee 降为 configured empirical confidence policy；
4. 审计 `L_i>T_t` 的 strict inequality、tie handling、至少 k 个 witness 和 fallback；
5. 回归测试证明关闭 SafeIn 不改变 candidate membership/exact verification 路径。

### NoOverlap 的最终优先级

`NoOverlapAsyncFinal` 降为 **P1**。只有在作者坚持把“cross-probe CPU/I/O overlap 的独立性能收益”写进 Abstract、贡献列表或主结论时，它才自动升级为 P0，并替换 P0-3 中除 correctness contract 以外的工程审计时间。

更好的两周策略是：正文把异步 issuance 写成第二条贡献的实现机制，不给它独立性能百分比；这样无需为了一个非核心因果 claim 牺牲 layout–span interaction。

## 3. 可直接进入 Abstract 的英文表述

> Vector search engines commonly separate ANN candidate generation, raw-vector verification, and record materialization behind identifier-based interfaces, preventing candidate evidence from shaping downstream I/O and preventing returned vector bytes from serving record retrieval. RecordGate closes this gap with two coordinated mechanisms. First, a one-sided, confidence-budgeted controller combines probabilistic distance bounds with a conservatively tightening top-k upper frontier to eliminate candidates whose raw vectors need not be verified, conditional on the configured query-level bound event. Second, a span-reusable execution substrate co-locates eligible payload bytes with raw vectors and coalesces currently admitted verification reads under a hard vector-byte amplification bound. For each fixed-order, tile-local run, RecordGate solves the lexicographic objective of minimizing read requests and then physical bytes exactly in $O(n\log n)$ time. Span completions expose inline payload bytes that were already returned by these vector reads as reusable views, and the system fetches only missing payload bytes after the candidate-set top-k is sealed. This cofetch is not speculative payload prefetch: no additional read is issued solely because a candidate is predicted to enter the final top-k.

如果 abstract 篇幅紧，可删去复杂度句，但不能删去 `fixed-order, tile-local` 和 `not speculative` 两个边界。

## 4. 可直接进入 Introduction 的两条 contribution

1. **One-sided, bound-guided verification control.**  
   > We introduce a confidence-budgeted controller that uses probabilistic distance intervals and a conservatively tightening top-k upper frontier to adapt raw-vector verification at candidate granularity. Conditional on the configured query-level bound event, an exclusion certificate is monotone as the candidate stream grows. The guarantee is scoped to the ANN-generated candidate set; candidate-generation recall remains orthogonal.

2. **Span-reusable physical execution.**  
   > We design a size-adaptive inline/external record substrate and an amplification-bounded cofetch path for currently admitted vector-verification reads. Within each fixed-order, tile-local run, an exact $O(n\log n)$ endpoint-dominance solver minimizes the lexicographic pair of read requests and physical bytes under the frozen per-span vector-byte admission rule. Returned spans expose already covered inline payload bytes as reusable materialization views; unlike speculative prefetch, this mechanism never issues extra payload I/O based solely on predicted top-k membership.

这两条足够。不要把 exact planner 或 layout 再拆成第三条 headline contribution。它们分别是第二条贡献中的 planner 与 substrate。

## 5. 对三份主线程草稿的必须修正项

### 5.1 `CLAIMS_MATRIX_CN.md`

1. **C1 暂时不应标为无条件“可直接主张”。** 在 query-level joint confidence/failure-budget audit 完成前，应改为“收窄后可主张”：NoSafeOut 支撑 verify-all work reduction；“不必要”只在配置的联合 bound event 下成立。
2. **C5 的措辞需更精确。** 不是“early SafeIn membership 被后续候选推翻”，而是“early inclusion condition 不是对最终 membership 的扫描单调证书”；候选实际 membership 未必改变。
3. **C6 要明确 amplification 的口径。** 是 per-span real vector-byte amplification，不是 total query bytes，也不约束 final payload fetch。
4. **C8 的正文安全表述仍过强。** `removes the quadratic planning cost` 暗示当前 $O(n^2)$ 是实际瓶颈；实际 run 很短，且 GE QPS 低于 GV。应改为“matches the frozen exact objective in $O(n\log n)$ worst-case time with about 0.06 ms/query measured overhead”。
5. **C9 在 NoOverlap 未完成前不要进入最终保留主张。** pipeline 可作为 implementation mechanism，不能在 evaluation claim 中承诺量化 overlap。
6. **C10 需要区分 covered、registered 与 consumed bytes。** 只有最终 top-k materializer 实际消费的 view 才能证明 final work reduction。
7. **最终保留主张第 3 条不应叫 contribution。** Evaluation 不是技术贡献；且其中的 pipeline overlap 尚未建立。保留为 evaluation RQ 即可。
8. **增加 candidate-set correctness 边界。** SafeOut 不修复 IVF candidate generation miss；这一点应成为矩阵中的独立 contract，而不只藏在正文。

### 5.2 `MINIMAL_EXPERIMENT_PLAN_CN.md`

1. **当前 P0 只列 Fixed-R 和 NoOverlap，优先级不对。** 应把 layout×span 两因素 interaction 升为 P0，把 NoOverlap 降为 P1，除非作者坚持 headline overlap claim。
2. **E4 所谓“增加结果导出而非新 sweep”需要审计 counters 是否已经存在。** 若没有 eligible/covered/registered/consumed 的完整 counters，就需要一次受控复跑，不能把缺失证据包装成离线导出。
3. **Fixed-R 必须冻结候选生成与物理执行路径。** 否则 recall/work 差异可能来自不同 nprobe、排序或 reader；还应报告完整 Pareto，而不是只挑 matched point。
4. **补 SafeIn-free configuration/correctness audit。** 当前计划只说“不物理删除代码”，没有要求 artifact telemetry 证明默认论文路径没有隐式进入 credit/tail/prefetch。
5. **把统一 cache/warmup/paired protocol 写进每个 P0 的共同合同。** 旧结果已经显示 cache conditioning 会改变绝对 QPS，不能只在总原则中笼统提及。
6. **Recall 指标需要与实际 top-k 配置对齐。** 不要机械并列 Recall@10/Recall@50；应预先指定主指标，并给 matched-recall 置信区间。
7. **E7 的累加式 interaction table 不能混用历史 full-stack cell。** 如果没有完全相同的 planner/layout/reader/cache 合同，就应缩为两因素实验，而不是强行补一张看似完整的瀑布表。

### 5.3 `PAPER_NARRATIVE_REWRITE_CN.md`

1. **把 `exact frontier` 改为 `conservative top-k upper frontier`，除非阈值确实由已 exact-verified distances 构成。** 当前上下文定义的是 k-th upper bound，不能混用术语。
2. **把 `remaining mandatory vector reads` 改为 `currently admitted verification reads`。** 一个当前 Uncertain candidate 可能随着 frontier 收紧而稍后可剪枝；早发读取可能增加 work，但不造成 accuracy error。
3. **删除“可行端点单调性”表述。** 旧分析已经说明 admission 未必 prefix-closed。只写“一维 endpoint-dominance recurrence”及其适用的冻结 admission 即可。
4. **Introduction 不要采用三条式贡献。** 该版本把 exact planner 和 format/execution 人为拆开，会强化“标准 DP 优化 + 常规 inline layout”的拼装印象。摘要和 Introduction 都使用两条式，方法章节再拆组件。
5. **`O(n\log n)` 的 exact 边界还应增加 `frozen admitted read set`。** 它不是对 evolving frontier、跨 tile、跨 query 或端到端 latency 的全局最优。
6. **异步提交目前只能写机制，不能写独立性能归因。** 在 NoOverlap 控制前，不要暗示 Full/NewNoPipeline 已证明 CPU/I/O overlap 的收益。
7. **natural cofetch 的触发语义应更硬。** 明确“不会因候选 top-k likelihood 单独发出额外 payload read”；covered bytes 只有经 extent validation 和 final consumption 后才算 reuse。
8. **SafeIn 的理论描述不要写成 label 必然错误。** 推荐写“lacks a scan-monotone final-membership certificate and failed to provide stable empirical benefit”。
9. **补 candidate-set scope 与 prefix storage semantics。** top-k sealed 是 candidate-set top-k；inline prefix 是否复制、external suffix locator、一致性与 view lifetime 必须在方法或 implementation 中落地。

## 6. 最终 Verdict

| 决策 | Verdict | 说明 |
|---|---|---|
| SafeIn 从 paper 主方法/贡献/主图/主结果移除 | **YES** | 立即执行；仅在 appendix/design alternative 中保留简短负结果 |
| SafeIn 在论文默认运行路径关闭 | **YES** | eager、cold-prefix、SE credit、tail 全关；natural span reuse 保留 |
| 两周内物理删除全部 SafeIn code | **NO** | 风险高、收益低；先隔离 flag、断言和 telemetry，投稿冻结后再清理 |
| 新的两条式论文叙事 | **REVISE, THEN ACCEPT** | 方向接受；完成三个 P0 或相应收窄 claim 后可形成 weak-accept 级故事 |

最终推荐的一句话主线是：

> RecordGate uses one-sided bound evidence to avoid unnecessary candidate verification, then turns the currently admitted vector reads into amplification-bounded spans whose naturally covered inline payload bytes can be reused for record materialization—without speculative membership-driven payload I/O.

本轮评审已收敛。继续为 SafeIn 调参不会提高论文接受概率；剩余工作应全部转向 fixed-R、公平的 layout–span interaction，以及 correctness/artifact contract。
