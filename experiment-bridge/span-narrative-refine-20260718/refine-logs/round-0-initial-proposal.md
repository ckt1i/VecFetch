# Research Proposal: Record-Aware Amplification-Bounded Co-fetch for RecordGate

## Problem Anchor

- **Bottom-line problem**: 在 record-return 向量检索中，把不可避免的原始向量验证读取转化为受控、可复用的连续 co-fetch，在不改变候选 membership 与 exact verification 语义的前提下，减少小规模随机 I/O 请求；并据此形成一条可被实验直接支撑的论文创新主线。
- **Must-solve bottleneck**: 分离的 vector/payload 读取使系统在候选验证与结果物化阶段产生大量小读；现有 streaming greedy 虽能合并读取，却不保证在固定有序 run 和读放大约束下得到请求数最少、字节数次少的分组，也没有严格利用 span 已覆盖的、可能有用的 inline payload bytes。
- **Non-goals**: 本轮不重新设计 ANN、RaBitQ/SafeOut 或 SafeIn 分类器；不把固定顺序连续分段包装成 NP-hard 装箱；不设计跨查询、跨 tile 的全局布局优化；不声称单次大读在所有设备和负载上总是优于小读；不让 co-fetch 或 SafeIn 改变最终结果正确性。
- **Constraints**: 论文与补实验剩余时间不足两周；复用当前 `.clu` 地址元数据、packed/split layout、64 KiB tile、async/serial reader、现有数据集与结果；优先采用无需训练、可解释、确定性的算法；所有正文结论必须区分已验证的 GE/vector-only exact 与仍需因果消融确认的 SE/SafeIn-aware 扩展。
- **Success condition**: 主方法能被精确定义为一个固定有序 run 上的受约束分段问题；exact planner 与独立二次 oracle 一致并以可接受开销运行；NoSpan、greedy/exact、Combined/NoCombine 的结果分别支撑请求合并、算法最优性和格式—执行协同。SafeIn-aware 只有在 paired 端到端结果、credit/reuse 遥测和 NoCombine 零 credit 边界共同通过时才进入主文，否则降为可选扩展或附录。

## Technical Gap

Record-return ANN 查询并不在候选 id 产生时结束。通过 bound-guided access control 排除不需要验证的候选后，剩余候选仍必须读取 raw vector 进行 exact verification，最终 top-k 还需要 payload。传统的 vector/payload 分离访问把这些工作表现为大量 candidate-local 小读；简单地把相邻向量扩成 span 虽能减少请求，但存在三处缺口。

第一，物理格式与读取策略通常分开设计。单纯的 payload inline/external 分层不是新点，单纯的 I/O coalescing 也不是新点；缺失的是一个明确的接口，使 mandatory vector span 穿过的 inline payload bytes 能被 query-local view 直接复用，同时让大 payload 不污染 vector 热路径。

第二，现有 streaming first-failure greedy 只是一种低开销执行策略。由于可行边不必对区间终点前缀封闭，它不能保证固定有序 run 上的最少请求分段。把它写成“接近最优算法”缺少理论和实验依据；把问题包装成 NP-hard 装箱则与连续区间分段的真实结构不符。

第三，旧 SafeIn 叙述把高置信候选提前升级为独立 full-record read。最新 paired 结果表明，该 eager 路径会把候选移出可合并的 vector span，并在四个数据集上降低中位 QPS。SafeIn 若继续存在，应只给“当前 span 确实覆盖、completion 确实可复用”的 inline bytes 提供折扣后的 useful-byte credit，而不是并列的第二套预取路径。

相邻工作已经覆盖 SSD ANN 的 I/O 合并、workload-aware block layout、page co-location、large-value out-of-line 和 hybrid value placement。因此可守的 gap 不是任何一个单点，而是：**面向 candidate-to-record 路径，把 size-adaptive record layout、硬读放大合同、payload view reuse 与精确分组算法闭合成一个可验证的格式—执行协同机制。**

## Route Comparison

### Route A: 仅包装 streaming greedy

保留 vector-only greedy，用“bounded streaming coalescer”表述。优点是 `O(n)`、实现最简单、绝对规划开销最低；缺点是没有最优性保证，论文只能把算法当作工程 heuristic。它适合作为 baseline 和极低延迟 fallback，不适合承担新增理论点。

### Route B: 格式感知的 exact bounded co-fetch

冻结同一个存储/edge contract，把每个 tile 内的有序候选 run 建模为连续区间分段，按 `(physical requests, physical bytes)` 字典序优化；利用 admission inequality 的一维 endpoint-dominance 结构，把朴素 `O(n^2)` DP 化为 `O(n log n)` exact planner。SafeIn-aware credit 是同一模型的条件性参数化扩展。

选择 Route B。原因不是它在 QPS 上大幅战胜 greedy——当前证据只支持非劣——而是它以约 `0.06 ms/query` 的规划代价给出明确、可复现的 optimality contract，并让格式、I/O 预算和执行计划形成一条完整机制链。Route A 保留为消融与 fallback。

## Method Thesis

- **One-sentence thesis**: RecordGate co-designs a size-adaptive record substrate with an amplification-bounded exact span planner, turning mandatory raw-vector I/O into request-efficient, payload-reusable co-fetch without changing exact verification semantics.
- **Why this is the smallest adequate intervention**: 只新增 inline/external 物理合同、一个标量 useful-byte credit 和固定 run 上的 exact partition；不改变 ANN、置信判断、候选顺序或最终 membership。
- **Why this route is timely**: 这是存储/数据库系统问题，不需要 LLM、RL 或学习式 cost model。确定性 edge contract 和 exact solver 更适合不足两周的实现与论文周期，也更容易做因果消融。

## Contribution Focus

- **Dominant contribution**: **Record-aware amplification-bounded co-fetch**。把 size-adaptive record format 与固定有序 run 上的受约束物理分段统一起来，并推导 `O(n log n)` endpoint-dominance exact planner，在硬放大约束下按请求优先、字节次优得到模型内最优计划。
- **Optional supporting contribution**: **Confidence-weighted useful-byte credit**。仅对 span 内部成员、预规划可知、物理上被当前 span 覆盖且 completion 可建立 payload view 的 SafeIn inline bytes 计入折扣 credit；是否进入正文由端到端与 NoCombine 因果证据决定。
- **Explicit non-contributions**: 不声称 adaptive inline/external placement 单独新颖；不声称 I/O coalescing 单独新颖；不声称该算法对 query-global、跨 tile 或设备 latency 最优；不声称 exact 比 greedy 更快；不再把 eager SafeIn full-record prefetch 当创新点。

在整篇 RecordGate 中，SafeOut 仍回答“哪些候选根本不必读”，本提案回答“无法避免的验证读取如何被组织成有用、受约束的物理 I/O”。两者构成同一 candidate-to-record access-control 主线的逻辑层与物理层，而不是两个互不相关的系统。

## Proposed Method

### Complexity Budget

- **Frozen / reused**: ANN 候选与排序、RaBitQ/SafeOut、exact verification、`.clu` `AddressEntry{offset,size}`、64 KiB tile、async/serial reader、final top-k materialization。
- **New trainable components**: 0。
- **New deterministic components**: size-adaptive record contract；span-resident payload view；exact planner；可选的 SafeIn credit 字段与统计。
- **Intentionally excluded**: workload-aware record reordering、跨 tile planning、learned latency cost、per-candidate learned `rho_k`、endpoint tail state、跨查询 cofetch、eager SafeIn full-record read。

### System Overview

```text
ANN candidates
  -> bound-guided logical access control
       -> Prune
       -> mandatory exact-verification candidates
  -> decode resident {tile, vector_offset, record_extent, inline/external}
  -> sort/partition each fixed same-tile run
       -> exact vector-only admission (default GE)
       -> optional SafeIn-aware admission (conditional SE)
  -> issue one physical read per selected span
  -> expose raw-vector slices for exact verification
  -> retain covered inline payload slices as query-local views
  -> seal exact top-k
  -> fetch only payload bytes still missing
```

### Size-Adaptive Span-Reusable Record Substrate

小 payload 与 raw vector、固定头部/descriptor 连续存储，使 mandatory vector span 有机会自然穿过并携带 record bytes；大 payload 存在 external region，cluster entry 只保留 locator，避免单个大对象把 vector scan 的物理 extent 拉长。这里的“adaptive”是按 record extent 决定 inline/external，而不是运行时任意重排。

读取完成后，系统按同一 offset/length contract 建立两类 slice：raw-vector slice 用于 exact verification；被 span 实际覆盖的 inline payload slice 成为 query-local reusable view。padding、descriptor、metadata miss、sidecar/external bytes 或未被物理 span 覆盖的 tail 都不能伪装成 useful payload。

### Amplification-Bounded Span Model

对一个固定 same-tile ordered run，令第 `k` 个 raw vector 的起始偏移为 `x_k`，固定向量长度为 `v`。把连续候选 `i..j` 合为一个 span 时：

\[
B(i,j)=x_j+v-x_i,\qquad V(i,j)=v(j-i+1).
\]

`B` 是实际物理读取字节；`V` 是无合并时必须读取的 raw-vector useful bytes。vector-only 默认 admission 为：

\[
B(i,j)\le \alpha V(i,j).
\]

`alpha` 是硬字节放大合同，不是经验性的 latency 模型。当前默认 `alpha=1.5`，即 planner 最多为 mandatory vector useful bytes 接受 50% 的 span extent 放大。tile 边界、最大单次长度和 reader 合法性仍是执行保护，不进入“系统普适最优”叙述。

若启用 SafeIn-aware 扩展，planner 前已知每个候选的 label 与可复用 inline extent。定义：

\[
s_k=\mathbf 1[SafeIn_k]\cdot inlineReusableBytes_k,
\qquad U_t=\sum_{k=1}^{t}s_k,
\]

并只给区间内部成员 credit：

\[
S(i,j)=U_{j-1}-U_{i-1}.
\]

统一 admission 变为：

\[
B(i,j)\le \alpha\bigl(V(i,j)+\rho S(i,j)\bigr),\qquad 0\le\rho\le1.
\]

`rho` 是 utility discount，不是置信概率。它只决定多少已覆盖 inline bytes 可以作为“有用读取”抵消 gap；它不改变 `B`、不减少已发出的物理字节，也不保证这些 payload 最终进入 top-k。当前低权重候选是 `rho=0.1`。

### Exact Endpoint-Dominance Planner

对每个可行区间 `(i,j)` 建立一条从 prefix `i-1` 到 prefix `j` 的边，边成本为 `(1,B(i,j))`。目标是在前缀 DAG 上最小化：

\[
\left(\#requests,\sum B(i,j)\right)
\]

的字典序值。朴素 DP 为 `O(n^2)`。利用固定 `alpha,rho` 与 prefix credit，admission 可改写为一维 dominance：

\[
x_j+v-\alpha v(j+1)-\alpha\rho U_{j-1}
\le
x_i-\alpha vi-\alpha\rho U_{i-1}.
\]

因此，对每个终点 `j`，只需在满足标量 start key 不小于 endpoint threshold 的起点中查询最佳 prefix-DP state。对 start key 坐标压缩，并用 Fenwick tree 维护 suffix minimum，即可得到 `O(n log n)` 时间、`O(n)` 空间的 exact planner。比较键依次为请求数、累计物理字节和确定性 tie-break。

该 optimality 只针对：固定候选顺序、固定 tile/run、固定 admission、固定字典序目标。它不是设备 latency-optimal，也不优化跨 run 并发。`m<=8` 使用相同语义的 bounded direct DP fast path；greedy 则作为 `O(n)` baseline/fallback。

### Inference / Query Execution Path

1. SafeOut 等逻辑规则冻结需要 exact verification 的候选集合。
2. 从 resident metadata 解码物理地址、record extent 与 inline/external 状态。
3. 按 tile 和 offset 形成 fixed ordered runs。
4. GE 运行 vector-only exact admission；只有在 SafeIn 证据通过时才切换 SE。
5. reader 按 plan 发起 span requests；planner groups 必须等于 issued vector requests，planned bytes 必须等于 issued vector bytes。
6. completion 同时暴露 vector slice 与合法 payload view；exact verification 决定 membership。
7. top-k sealed 后仅补读缺失 payload。独立 eager SafeIn full-record prefetch 默认关闭。

### Failure Modes and Diagnostics

- **Exact 请求减少但 QPS 变差**: 同时报告 planner ms、requests、bytes、avg/p99；若特定设备/负载对 CPU 更敏感，可回退 GV，但正文仍把 GE 定位为 theory-preferred 而非 QPS winner。
- **SafeIn credit 读取更多无效 bytes**: 报告 discounted/covered/consumed credit、retained bytes、reuse hits 与 pool misses；未通过 paired gate 时设 `rho=0`。
- **NoCombine 仍获得 credit**: 这是 contract bug。sidecar/external/padding 必须为 0，并由单测与 NoCombine 遥测验证。
- **算法只在小 run 上显得便宜**: 报告实际 `m` 分布与 `m=1..32` microbenchmark；不把当前 `m<=21` 外推为任意规模结论。
- **大读并不适合某设备**: 保留 `alpha` 和 greedy fallback；结论限定为测试设备、请求开销主导的 record-return workload。

### Novelty and Elegance Argument

论文不把“inline/external layout”“coalescing”或“DP optimization”拆成三条弱创新。它们组成一个闭环：格式暴露可复用 bytes，放大合同定义可接受 I/O，exact planner在该合同内选择最优分段，completion 把覆盖 bytes 变成实际 payload view。任意删除其中一个环节都会让主张退化为已有的通用思想。

与 SSD ANN I/O coalescing 的区别是目标不仅是向量页，而是 candidate-to-record 的 vector verification 与 payload materialization；与 workload-aware block layout 的区别是当前不重排磁盘布局、不求解 NP-hard offline placement；与 hybrid value placement 的区别是 inline 决策为 mandatory vector span 提供可验证复用接口；与 page co-location/prefetch 的区别是有显式硬放大合同、模型内 exact partition 和 zero-credit boundary。

算法层面应使用“we derive an exact `O(n log n)` planner for our fixed-order admission model”，而不是“we solve a new general optimization problem”。创新强度来自 co-design 及其可执行 contract，endpoint-dominance 是保证该 contract 不必依赖 heuristic 的关键技术。

## Paper Narrative Rewrite

### Core story

建议把旧的四动作叙述改成两层：逻辑层决定 `Prune / Verify / Materialize-if-selected`；物理层把所有 mandatory `Verify` 组织成 bounded spans。正文顺序是：

1. record-return search 的成本不只来自 ANN，而来自 candidate-to-record 路径；
2. bound-guided control 先消除不必要的 verification；
3. 剩余 vector reads 无法消除，但可以通过 record-aware bounded co-fetch 重新组织；
4. small payload inline 使读放大中的一部分字节可成为潜在结果，而 large payload external 保护热路径；
5. exact planner 在硬放大约束内最小化 requests/bytes；
6. optional SafeIn credit 只调整“被覆盖字节的 utility”，不再触发独立 eager read；
7. exact verification 和 final fetch 保持 correctness boundary。

建议的一句话：

> RecordGate first avoids unnecessary exact verification through bound-guided access control, then converts unavoidable vector reads into amplification-bounded, record-aware co-fetches whose inline payload bytes can be reused during final materialization.

### Contribution wording

整篇论文可保留两条主贡献和一条实现/评估总结：

1. **Bound-guided candidate-to-record access control**：决定哪些候选必须验证，哪些可在当前 exact frontier 下跳过；
2. **Record-aware amplification-bounded co-fetch**：共同设计 size-adaptive record substrate 与 `O(n log n)` exact planner，在固定有序 run 上以硬放大合同得到请求优先、字节次优的最优物理计划，并复用 span-resident payload；
3. **Evaluation（不要包装成第三个独立机制）**：在五类 record-return workload 上分解 access reduction、layout、span、planner 与 materialization policy 的收益和边界。

SafeIn-aware 若通过剩余 gate，只作为贡献 2 的一句扩展：

> The same formulation optionally discounts physically covered inline bytes from high-confidence candidates, allowing the planner to trade a small amount of extra I/O for fewer requests without introducing a separate eager-prefetch path.

若未通过，则正文完全使用 vector-only GE，上句移到 appendix/limitations。

## Claim-Driven Validation Sketch

### Claim 1: Bounded co-fetch 是有效的物理机制，exact planner 在模型内最优且端到端非劣

- **Minimal experiment**: `NoSpan -> GV -> GE`，核心 ESCI/MSMARCO，固定 binary、topk/nprobe、五次 paired；加独立 `O(n^2)` oracle differential test 与 planner microbenchmark。
- **Baselines / ablations**: NoSpan、streaming greedy GV、exact GE。
- **Metric**: requests/query、physical bytes/query、actual amplification、planner ms、QPS、p99、recall/probed/reranked、fallback。
- **Decisive evidence**: NoSpan→GV 显著减请求且提高 QPS；GE 100% 匹配 oracle、请求不多于 GV、字节在同请求数下不多于 GV，QPS/p99/bytes 通过预设非劣门禁。
- **Existing support**: NoSpan→GV 在 ESCI/MSMARCO `nprobe=96` 的请求下降约 55.20%/68.05%、QPS +1.82%/+4.81%；GE-GV 核心点 QPS约 -0.89%/-0.62%，请求约 -1.49%/-0.22%，planner 约 `0.06 ms/query`，并通过 oracle 与 43/43 tests。

### Claim 2: 格式—执行协同使读放大字节成为可复用 payload，而不是纯 gap

- **Minimal experiment**: Combined 与 NoCombine 在同一 GE planner 下 paired；报告 plan、issued bytes、payload view/reuse 和 final missing-payload reads。
- **Baselines / ablations**: packed Combined、controlled split NoCombine；真实 FlatStor/Lance 只用于系统基线，不与机制消融混写。
- **Metric**: QPS、p99、vector/total requests、bytes、reuse bytes/hits、final payload requests、recall。
- **Decisive evidence**: Combined 保持相同 membership/recall，产生非零可消费 inline views、减少最终物化工作，并在代表性 workload 上端到端改善；NoCombine credit 必须为零。
- **Existing support**: 当前 GE representative points 中 Combined 相对 NoCombine QPS 为 ESCI +2.10%、COCO +9.58%、MSMARCO +10.81%、Vox +6.12%，recall 一致；旧完整 sweep 可作为覆盖，最新点作 binary drift anchor。

### Conditional Claim 3: SafeIn-aware credit 能进一步减少请求

- **Minimal experiment**: `SE(rho=0.1) vs GE` paired，并在 Combined/NoCombine 两种 layout 下核对 credit causality；不重新运行独立 eager prefetch 作为主方法。
- **Metric**: request/byte/QPS/p99 delta、covered/discounted/consumed credit、reuse、NoCombine zero-credit。
- **Decisive evidence**: 至少两个机制命中数据集稳定减请求，QPS/p99/bytes 在门禁内，Combined 有 credit 且 NoCombine 为零；否则不进入主贡献。
- **Current boundary**: `rho=0.1` 在 ESCI/MSMARCO 进一步减请求约 0.69%/0.22%，QPS 回退约 0.40%/0.27%，尚缺 SE NoCombine 因果验证。独立 eager SafeIn prefetch 四数据集 QPS 均回退，必须从主线删除。

## Experiment Handoff Inputs

- **Must-prove claims**: exact optimality 的严格范围；span 机制的 request/QPS 主收益；Combined layout 对 payload reuse 的因果作用。
- **Must-run ablations**: 复用现有 NoSpan/GV/GE 和 GE NoCombine；只新增 SE `rho=0.1` 的 Combined/NoCombine representative paired gate，若失败立即降级。
- **Critical datasets / metrics**: ESCI、MSMARCO 为 exact/SafeIn 核心；COCO 为 layout 正向与 eager-prefetch 边界；Vox 为 zero-credit/external boundary。报告 recall、QPS、p99、requests、bytes、amplification、planner、credit/reuse。
- **Highest-risk assumptions**: exact 的增量请求收益较小；SafeIn credit 可能只是“少请求、多字节”的 Pareto 点；现有 NoCombine 是受控机制对照，不能替代真实后端基线。

## Compute & Timeline Estimate

- **Training/GPU hours**: 0。
- **Implementation**: exact/SE 已完成；只需 0.5--1 天补 SafeIn-aware NoCombine gate 与统计检查。
- **Core reruns**: 1--2 天完成必要 paired representatives；完整旧 sweep 复用并以 current-binary anchors 防漂移。
- **Narrative/method update**: 1--2 天更新 `NARRATIVE_REPORT`、method contract、Introduction/System Design/Experiments claim ledger。
- **Stop rule**: 不为 SafeIn-aware 扩展追加大规模 sweep；一次因果 gate 失败即冻结 GE/vector-only exact 主线。
