# SafeIn 移除后的论文叙事改写

日期：2026-07-18

## 一句话主线

RecordGate 使用概率距离界与不断收紧的 conservative top-k upper frontier 避免不必要的候选验证，再把当前已接纳的 vector-verification reads 组织为受读放大约束的连续 spans，并复用这些 spans 已经覆盖、且最终 materializer 实际消费的合法 inline payload 字节，从而跨越 vector index 与 record store 的传统物理边界。

## 推荐的问题定义

向量数据库的候选生成、raw-vector verification 与 record materialization 通常被视为三个顺序阶段。候选侧的证据不会系统地约束后续读取工作，vector reads 也不会成为 payload materialization 可复用的物理输入。结果是系统需要在静态验证预算、碎片化小读和末端 payload 补读之间做彼此割裂的权衡。

不要把相关工作概括成“所有传统系统都使用固定阈值”；更安全的表述是：

> Static rerank budgets allocate the same verification allowance without exploiting candidate-specific uncertainty and the evolving top-k frontier, while conventional vector/record boundaries prevent mandatory vector I/O from being reused for record materialization.

## 两条主机制

### 机制 A：Bound-Guided Verification Control

RaBitQ 为候选提供概率距离区间。RecordGate 在线维护已见候选的 conservative kth upper frontier；随着扫描推进，该 frontier 单调收紧。当候选 lower bound 已差于 frontier 时，在配置的 query-level bound policy 下，它可以被永久排除，无需读取 raw vector。该合同仅针对 ANN/IVF 已生成的候选集，candidate-generation recall 与之正交。

该机制只保留 SafeOut。论文不再定义 early SafeIn membership，不再根据它发起 payload I/O，也不再把候选置信度写成 payload prefetch 来源。

推荐名称：

- `Bound-Guided Verification Control`
- `Confidence-Budgeted Candidate Elimination`
- `Monotone Exclusion Frontier`

不推荐继续使用：

- `SafeIn/SafeOut dual classification`
- `error-free pruning`
- `dynamic payload prefetch`（如果指 SafeIn）

### 机制 B：Amplification-Bounded Span Execution over Reusable Records

对当前已接纳执行 raw-vector verification 的候选，RecordGate 按物理地址排序，并在固定 tile 内将连续候选划分成 spans。一个 span 可以覆盖候选间的 gap，但必须满足配置的 per-span real-vector-byte amplification 和最大读取约束；该放大约束不包含 final payload fetch。目标按字典序最小化 `(read requests, physical bytes)`。

朴素 DP 枚举所有区间端点，复杂度为 `O(n^2)`。RecordGate 使用一维 endpoint-dominance recurrence，在 `O(n log n)` worst-case time 内求出冻结模型的 exact partition。这里的 `exact` 只针对 frozen admitted read set、固定顺序、同 tile 和冻结的 lexicographic objective，不应扩展为 evolving frontier、跨 tile/跨 query 或端到端 latency 的全局最优。

这些 spans 可在 IVF probing 期间异步提交。现有对照尚未隔离 cross-probe overlap 的独立收益，因此它首先是执行机制。record layout 将小 payload 或配置的 payload prefix 与 raw vector 共置；只有 extent 校验通过、span 已覆盖对应字节且 final candidate-set top-k materializer 实际消费时，才计为 reuse，并在 top-k sealed 后补读缺失部分。

这里应使用 `cofetch` 或 `natural reuse`，而不是 `SafeIn prefetch`：payload 字节不是因为预测某个候选会进入 top-k 而额外读取，而是已经落在受限放大下、当前已接纳的 vector-verification span 中。

## 推荐贡献表述

### 两条式版本（用于 Abstract 与 Introduction）

1. **Evidence-to-I/O control**：bound evidence 决定哪些 raw vectors 是 mandatory。
2. **I/O-to-record reuse**：bounded exact spans 执行这些 mandatory reads，并把覆盖的 inline payload 转化为 final materialization reuse。

摘要与 Introduction 都只采用两条式，避免把 exact planner 与 layout/execution 人为拆开、形成“标准 DP 优化 + 常规 inline layout”的拼装印象。方法章节再把 planner、substrate 和 async execution 分开说明。

### 可直接采用的 Abstract 核心段

> Vector search engines commonly separate ANN candidate generation, raw-vector verification, and record materialization behind identifier-based interfaces, preventing candidate evidence from shaping downstream I/O and preventing returned vector bytes from serving record retrieval. RecordGate closes this gap with two coordinated mechanisms. First, a one-sided, confidence-budgeted controller combines probabilistic distance bounds with a conservatively tightening top-k upper frontier to eliminate candidates whose raw vectors need not be verified, conditional on the configured query-level bound event. Second, a span-reusable execution substrate co-locates eligible payload bytes with raw vectors and coalesces currently admitted verification reads under a hard vector-byte amplification bound. For each fixed-order, tile-local run and a frozen admitted read set, RecordGate solves the lexicographic objective of minimizing read requests and then physical bytes exactly in `O(n log n)` time. Span completions expose inline payload bytes already returned by these vector reads as reusable views, and the system fetches only missing payload bytes after the candidate-set top-k is sealed. This cofetch is not speculative payload prefetch: no additional read is issued solely because a candidate is predicted to enter the final top-k.

### 可直接采用的 Introduction contributions

1. **One-sided, bound-guided verification control.** We introduce a confidence-budgeted controller that uses probabilistic distance intervals and a conservatively tightening top-k upper frontier to adapt raw-vector verification at candidate granularity. Conditional on the configured query-level bound event, an exclusion certificate is monotone as the candidate stream grows. The guarantee is scoped to the ANN-generated candidate set; candidate-generation recall remains orthogonal.
2. **Span-reusable physical execution.** We design a size-adaptive inline/external record substrate and an amplification-bounded cofetch path for currently admitted vector-verification reads. Within each frozen, fixed-order, tile-local run, an exact `O(n log n)` endpoint-dominance solver minimizes the lexicographic pair of read requests and physical bytes under the frozen per-span vector-byte admission rule. Returned spans expose already covered inline payload bytes as reusable materialization views; unlike speculative prefetch, this mechanism never issues extra payload I/O based solely on predicted top-k membership.

## 与原三点想法的逐项修正

### 原第 1 点

问题：`固定阈值缺乏理论依据、读取效率低和误差大` 过强；当前还没有 matched-recall fixed-R baseline。去掉 SafeIn 后也不能再写动态 prefetch。

改为：

> Static verification budgets cannot adapt work to per-candidate uncertainty or an evolving top-k frontier. RecordGate instead uses RaBitQ bounds to derive a confidence-budgeted, monotonically tightening exclusion frontier, avoiding raw-vector reads that are no longer necessary under the configured bound policy.

### 原第 2 点

问题：现有 Full/NewNoPipeline 控制并不只改变 overlap；`提前读取可能的 top-k 数据` 容易被理解为 SafeIn 预测。

改为：

> RecordGate can asynchronously issue currently admitted vector-verification reads during IVF probing. For each frozen, fixed-order, tile-local run, it formulates read coalescing as an amplification-bounded interval partition and solves the frozen request-first objective exactly in `O(n log n)` worst-case time.

在补完公平 pipeline 消融前，不给出纯 overlap 的独立性能百分比。

### 原第 3 点

问题：单独写“把小 payload 和 vector 放一起”容易与已有 co-location/layout 工作重叠，也可能被误解成无条件读取全部 payload。

改为：

> A size-adaptive record layout co-locates eligible payload bytes or prefixes with raw vectors. RecordGate does not issue an extra payload read solely because a candidate is likely to enter top-k; it registers a view only after extent validation and consumes covered bytes only for the sealed candidate-set top-k, fetching any missing payload afterward.

## SafeIn 的论文与代码处置

### 论文

- 从 abstract、introduction contributions、method 主路径、主图和主消融中删除。
- 在 design rationale 或 appendix 用一小段解释：early inclusion 非单调，且严格实验带来负收益/高无效字节比例。
- SafeIn-aware `SE`、cold prefetch、tail extension 不再进入系统默认配置。

### 代码

- 论文冻结前：默认关闭，保留实验开关和 counters，避免两周内大规模删除引入回归。
- artifact 配置：明确 `safein_prefetch=off`，并添加断言/日志证明论文运行未进入该路径。
- 论文提交后：再单独清理不活动路径；SafeOut 与通用 bound 计算不能被误删。

## 相关工作边界

- [FusionANNS (FAST '25)](https://www.usenix.org/conference/fast25/presentation/tian-bing) 已包含 heuristic reranking、redundant-aware I/O deduplication 和 optimized data layout；因此“合并读”或“布局优化”本身不足以作为新颖性主张。
- [VeloANN](https://arxiv.org/abs/2602.22805) 与 [Starling](https://arxiv.org/abs/2401.02116) 等工作也强调 page/block locality、reordered layout 与异步或 I/O-efficient execution。
- RecordGate 应突出的是完整闭环：`candidate-specific bound evidence → mandatory read set → exact amplification-bounded span plan → payload-byte reuse`，并用逐层消融证明每个箭头。

## 建议的章节顺序

1. **Introduction**：vector index/record store 边界造成的 evidence-to-I/O disconnect。
2. **Background and Motivation**：静态 verification budget、碎片化 mandatory reads、末端 materialization；SafeIn 失败仅作设计动机，不占主篇幅。
3. **Overview**：两阶段闭环和数据流。
4. **Bound-Guided Verification**：概率区间、monotone upper frontier、置信与 recall contract。
5. **Amplification-Bounded Span Planning**：模型、DP、endpoint dominance、复杂度与模型边界。
6. **Reusable Record Substrate and Pipeline**：layout、tile、async submission、reusable view、missing payload fetch。
7. **Evaluation**：end-to-end、fixed-R frontier、SafeOut、NoSpan/GE/GV、pipeline fair ablation、payload reuse。
8. **Discussion**：概率界、workload-dependent payload coverage、最大读取约束、SafeIn negative result。
