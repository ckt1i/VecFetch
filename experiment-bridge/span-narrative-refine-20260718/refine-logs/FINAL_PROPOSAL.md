# Research Proposal: A Span-Reusable Record Substrate with Amplification-Bounded Exact Co-fetch

## Problem Anchor

- **Bottom-line problem**: 在 record-return 向量检索中，把不可避免的原始向量验证读取转化为受控、可复用的连续 co-fetch，在不改变候选 membership 与 exact verification 语义的前提下，减少小规模随机 I/O 请求；并据此形成一条可被实验直接支撑的论文创新主线。
- **Must-solve bottleneck**: 分离的 vector/payload 读取使系统在候选验证与结果物化阶段产生大量小读；现有 streaming greedy 虽能合并读取，却不保证在固定有序 run 和读放大约束下得到请求数最少、字节数次少的分组，也没有严格利用 span 已覆盖的、可能有用的 inline payload bytes。
- **Non-goals**: 本轮不重新设计 ANN、RaBitQ/SafeOut 或 SafeIn 分类器；不把固定顺序连续分段包装成 NP-hard 装箱；不设计跨查询、跨 tile 的全局布局优化；不声称单次大读在所有设备和负载上总是优于小读；不让 co-fetch 或 SafeIn 改变最终结果正确性。
- **Constraints**: 论文与补实验剩余时间不足两周；复用当前 `.clu` 地址元数据、packed/split layout、64 KiB tile、async/serial reader、现有数据集与结果；优先采用无需训练、可解释、确定性的算法；所有正文结论必须区分已验证的 GE/vector-only exact 与仍需因果消融确认的 SE/SafeIn-aware 扩展。
- **Success condition**: 主方法能被精确定义为一个固定有序 run 上的受约束分段问题；exact planner 与独立二次 oracle 一致并以可接受开销运行；NoSpan、greedy/exact、Combined/NoCombine 的结果分别支撑请求合并、算法最优性和格式—执行协同。SafeIn-aware 只有在 paired 端到端结果、credit/reuse 遥测和 NoCombine 零 credit 边界共同通过时才进入主文，否则降为可选扩展或附录。

## Executive Decision

新增方案可以构成论文中的系统创新，但不应写成“自适应格式”和“新优化算法”两个平行贡献。推荐冻结为一个闭环贡献：

> **A span-reusable record substrate with amplification-bounded exact co-fetch.**

该贡献包含四个连续环节：

1. 小 payload 与 raw vector 共置，大 payload 外置；
2. mandatory vector reads 在真实 vector-byte amplification 上限内被合并；
3. exact fixed-run planner 消除 greedy 的 heuristic gap；
4. span completion 把实际覆盖的 inline payload 变成 reusable view，final materialization 只补读缺失部分。

GE 是当前正文默认。SafeIn-aware SE 不属于核心贡献；它只有在剩余因果门槛通过后，才能作为同一模型的 conditional Pareto extension。独立 eager SafeIn full-record prefetch 应退出推荐方法。

## Technical Gap

RecordGate 的逻辑访问控制先回答“哪些候选仍需 exact verification”。对剩余候选，raw-vector reads 是 correctness path 上不可避免的物理工作。逐候选访问会产生大量小 I/O；简单相邻合并可以降低请求数，却没有同时回答三个问题：允许多读多少真实字节、这些附带字节能否被 record return 使用、分组 heuristic 与模型最优解相差多少。

已有工作分别覆盖 SSD ANN I/O coalescing、page co-location、async prefetch、large-value out-of-line 和 hybrid value placement。因此，本工作的 novelty 不是任一单点，而是 candidate-to-record 路径上的格式—执行合同：格式暴露可复用 record bytes，硬放大合同限制真实 vector I/O，exact planner选择模型内最优分段，completion/final fetch 保证附带字节被正确复用或补齐。

## Method Thesis and Contribution Focus

- **Method thesis**: RecordGate combines a size-adaptive, span-reusable record substrate with amplification-bounded exact co-fetch, reducing requests for mandatory vector verification while reusing naturally covered inline payload bytes without changing exact-result semantics.
- **Dominant contribution**: span-reusable record substrate + bounded co-fetch execution contract。
- **Key enabling technique**: 为 fixed ordered run 推导 `O(n log n)` endpoint-dominance exact planner；它提供模型内最优性和确定性，不单独承担主要性能创新。
- **Conditional extension**: SafeIn-aware useful-byte credit；不影响 GE 主线是否成立。
- **Non-contributions**: 不把 inline/external placement、I/O coalescing 或 Fenwick DP optimization 单独声称为创新；不声称 latency-optimal、query-global optimal 或普适设备最优。

## Proposed Method

### Two-Layer Execution Model

```text
Logical access control
  candidates -> Prune or mandatory Verify

Physical access planning
  mandatory Verify candidates
    -> same-tile ordered runs
    -> GE bounded exact partition
    -> contiguous span reads
    -> raw-vector slices + covered inline payload views
    -> exact top-k
    -> final missing-payload fetch
```

逻辑层决定工作是否必要；物理层决定不可避免的工作如何组织。exact verification 始终是 membership 的唯一确认路径，co-fetch 与 payload reuse 只改变请求形状和数据到达时机。

### Span-Reusable Record Substrate

cluster entry 提供 raw-vector offset、record extent 和 payload locator。小 payload inline，使其可能位于相邻 mandatory vector reads 的连续 extent 中；大 payload external，避免大对象扩大 vector 热路径。

格式的关键合同不是简单的 size threshold，而是 completion view：span 返回后，系统必须能从 buffer 中建立经过 offset/length 验证的 raw-vector slice 和 inline payload slice。只有物理覆盖、metadata 一致、buffer lifetime 足够且 final materializer 可消费的 payload bytes 才算 reuse；padding、descriptor、external/sidecar、metadata miss 和未覆盖 endpoint tail 均不算。

### Vector-Byte Amplification Contract

对同一 tile 内按地址排序的 run，令第 `k` 个 raw vector 的起点为 `x_k`、长度为 `v`。区间 `i..j` 的物理读取和 mandatory vector bytes 为：

\[
B(i,j)=x_j+v-x_i,\qquad V(i,j)=v(j-i+1).
\]

GE 只允许满足下式的区间：

\[
B(i,j)\le\alpha V(i,j).
\]

因此每个 GE span 都满足：

\[
A_{vec}(i,j)=\frac{B(i,j)}{V(i,j)}\le\alpha.
\]

当前 `alpha=1.5` 是测试系统的 per-span vector-byte bound，不是设备普适最优值，也不约束 top-k sealed 后的 final payload fetch。

### Exact Fixed-Run Partition

把每个可行区间 `(i,j)` 建成 prefix DAG 中从 `i-1` 到 `j` 的边，边成本为 `(1,B(i,j))`。目标按字典序最小化：

\[
\left(\#physical\ requests,\sum physical\ bytes\right).
\]

朴素 DP 为 `O(n^2)`。GE 的 admission 可化为 start scalar 与 endpoint threshold 的一维 dominance 查询；对 scalar 坐标压缩并以 Fenwick tree 维护 suffix minimum，可在 `O(n log n)` 时间、`O(n)` 空间得到 exact 解。`m<=8` 使用同语义 bounded direct DP fast path，较长 run 使用 endpoint-dominance/Fenwick，GV 保留为 `O(n)` fallback。

exactness 严格限定于 fixed order、fixed tile/run、fixed admission 和上述字典序目标。主要端到端收益来自 NoSpan→bounded span；GE 的作用是消除 greedy heuristic gap、提供可审计最优性并进一步减少少量请求。不能写成“exact 比 greedy 更快”或“新的通用 I/O 最优算法”。

### Completion-Time Reuse

每个 span completion 同时暴露：

- raw-vector slices，用于 exact verification；
- 实际覆盖的 inline payload views，用于后续 materialization。

top-k sealed 后，materializer 先查询 query-local view registry，仅对缺失 bytes 发起最终读取。GE 本身只根据 vector offsets/bytes 规划，因此不能称为 layout-aware optimizer；格式与 planner 的结合点位于 span completion 和 final materialization。

### Conditional SafeIn-Aware Extension

若 planner 前有 SafeIn label，且 metadata 能给出 completion 可复用的 inline extent，定义：

\[
s_k=\mathbf1[SafeIn_k]\cdot inlineReusableBytes_k,
\qquad S(i,j)=\sum_{k=i}^{j-1}s_k.
\]

SE 使用 utility-adjusted admission：

\[
B(i,j)\le\alpha\bigl(V(i,j)+\rho S(i,j)\bigr).
\]

SE 只保证：

\[
A_{eff}=\frac{B}{V+\rho S}\le\alpha,
\]

不保证真实 `A_vec=B/V<=alpha`。因此必须同时报告 `A_vec` 与 `A_eff`。`rho` 是 utility discount，不是命中概率；credit 不减少物理字节，也不保证 payload 最终进入 top-k。endpoint、external/sidecar、padding 与 metadata miss 为零 credit。

当前 `rho=0.1` 只形成“少请求、略多字节、QPS 小幅回退”的 Pareto 点。独立 eager SafeIn prefetch 在最新四数据集 paired 实验中均不成立，推荐默认关闭。

## Paper-Facing Narrative

### Core paragraph

> RecordGate treats post-ANN execution as two coupled decisions. Its logical controller first removes candidates that do not require exact verification under the configured confidence policy. It then organizes the remaining mandatory vector reads through a size-adaptive, span-reusable record substrate. Small payloads remain adjacent to raw vectors, while large payloads are reached through external locators. Within each ordered tile-local run, an exact planner coalesces vector reads under a hard vector-byte amplification bound. The returned span buffers provide both vector slices for exact verification and reusable views over any inline payload bytes they already cover; after the exact top-k is sealed, RecordGate fetches only the missing payload bytes.

### Recommended contribution bullets

1. **Bound-guided candidate-to-record access control.** We formulate the post-ANN boundary as an online access-control problem that uses bounded distance evidence and the evolving exact frontier to avoid unnecessary raw-vector verification without making prefetch part of correctness.
2. **A span-reusable record substrate with amplification-bounded exact co-fetch.** We co-locate small payloads with raw vectors, keep large payloads external, and derive an exact `O(n log n)` planner that minimizes requests and then bytes for fixed ordered runs under a hard vector-byte amplification bound. Span completions expose covered inline payload as reusable materialization views.

不要把 evaluation、prefix-resident bits 或 SafeIn 再列成平行 headline contribution。prefix-resident 作为 deployment knob；SafeIn 只有通过 gate 时，才在第二条末尾补一句：

> The same formulation can conservatively credit physically covered inline bytes from high-confidence candidates, providing an optional request/byte Pareto point without a separate eager-prefetch path.

### Working one-sentence story

> RecordGate first avoids unnecessary exact verification, then turns unavoidable vector reads into amplification-bounded co-fetches whose naturally covered inline payload bytes can be reused for record materialization.

## Claim Boundaries

正文不得声称：

- 单次大读在所有设备/负载上总优于随机小读；
- 当前 span 是 NP-hard bin packing/knapsack；
- GE 是 record-aware/layout-aware optimizer；
- exact 比 greedy 更快或是主要 QPS 来源；
- 所有 final physical reads 都受 1.5× 上限；
- SafeIn prefetch 普遍有效；
- NoCombine 证明优于 FlatStor/Lance 等真实后端。

GE 的 “empirically non-inferior” 目前只能用于 ESCI/MSMARCO 的冻结核心点，不能外推到 Vox；Vox 应作为 CPU-sensitive boundary 或 GV fallback 案例。

## Minimal Claim-Driven Validation

### Claim 1: bounded span co-fetch 减少请求；exact 以低开销消除 heuristic gap

- **Experiment**: `NoSpan -> GV -> GE`，ESCI/MSMARCO `nprobe={96,192}` paired；oracle tests；exact plan-gap telemetry。
- **Metrics**: requests、bytes、`A_vec`、QPS、p99、planner ms、recall/probed/reranked、fallback、different-plan ratio、request/byte gap distribution、run-size distribution。
- **Existing evidence**: NoSpan→GV requests 约 `-53%~-68%`、QPS约 `+0.91%~+4.81%`；GE-GV 核心 QPS约 `-0.62%~-0.89%`、requests 不增加；planner约 `0.06 ms/query`；oracle 与 43/43 tests 通过。

### Claim 2: layout 使 span 放大字节可用于 record materialization

- **Experiment**: `Combined/NoCombine × NoSpan/GE` 最小两因素矩阵。
- **Metrics**: vector/payload requests and bytes、span-covered reusable bytes、actually consumed bytes、final missing-payload reads、QPS/p99、recall。
- **Decisive evidence**: GE 在两种 layout 都减少 vector requests；只有 Combined 产生合法 inline views，并相对自身 NoSpan cell 减少 final materialization work。
- **Existing evidence**: GE 下 Combined 相对 NoCombine 的 representative QPS 为 ESCI `+2.10%`、COCO `+9.58%`、MSMARCO `+10.81%`、Vox `+6.12%`，recall 一致；这些可作为 drift anchors，但单独不能证明 interaction。

### Conditional Claim 3: SafeIn credit 提供可选 request/byte Pareto 点

- **Experiment**: `SE(rho=0.1) vs GE` 在 Combined/NoCombine 下 paired；验证 zero-credit 与 consumed-credit。
- **Inclusion gate**: 至少两个机制命中数据集 requests 稳定下降且 QPS/p99/bytes 过门禁；Combined 有 credit/reuse、NoCombine 为零。
- **Current boundary**: ESCI/MSM requests约 `-0.69%/-0.22%`、QPS约 `-0.40%/-0.27%`，尚不足以进入主贡献。

## Two-Week Execution Boundary

- 必做：exact plan-gap telemetry；`Combined/NoCombine × NoSpan/GE` representative paired。
- 条件做：一次 SE NoCombine/consumed-credit gate；失败立即停止并冻结 GE。
- 复用：NoSpan anchors、GE/GV paired、oracle/CTest/CPU logs、GE NoCombine drift anchors、旧完整 NoCombine sweep。
- 不再做：大型 alpha/rho grid、完整四方案 sweep、独立 eager SafeIn prefetch、跨设备大矩阵。

最终论文主线不依赖 SafeIn 成败；剩余实验只决定是否增加一条 conditional extension，以及格式—执行 interaction 能写到多强。
