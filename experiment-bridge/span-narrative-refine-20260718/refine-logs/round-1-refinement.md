# Round 1 Refinement

## Problem Anchor

- **Bottom-line problem**: 在 record-return 向量检索中，把不可避免的原始向量验证读取转化为受控、可复用的连续 co-fetch，在不改变候选 membership 与 exact verification 语义的前提下，减少小规模随机 I/O 请求；并据此形成一条可被实验直接支撑的论文创新主线。
- **Must-solve bottleneck**: 分离的 vector/payload 读取使系统在候选验证与结果物化阶段产生大量小读；现有 streaming greedy 虽能合并读取，却不保证在固定有序 run 和读放大约束下得到请求数最少、字节数次少的分组，也没有严格利用 span 已覆盖的、可能有用的 inline payload bytes。
- **Non-goals**: 本轮不重新设计 ANN、RaBitQ/SafeOut 或 SafeIn 分类器；不把固定顺序连续分段包装成 NP-hard 装箱；不设计跨查询、跨 tile 的全局布局优化；不声称单次大读在所有设备和负载上总是优于小读；不让 co-fetch 或 SafeIn 改变最终结果正确性。
- **Constraints**: 论文与补实验剩余时间不足两周；复用当前 `.clu` 地址元数据、packed/split layout、64 KiB tile、async/serial reader、现有数据集与结果；优先采用无需训练、可解释、确定性的算法；所有正文结论必须区分已验证的 GE/vector-only exact 与仍需因果消融确认的 SE/SafeIn-aware 扩展。
- **Success condition**: 主方法能被精确定义为一个固定有序 run 上的受约束分段问题；exact planner 与独立二次 oracle 一致并以可接受开销运行；NoSpan、greedy/exact、Combined/NoCombine 的结果分别支撑请求合并、算法最优性和格式—执行协同。SafeIn-aware 只有在 paired 端到端结果、credit/reuse 遥测和 NoCombine 零 credit 边界共同通过时才进入主文，否则降为可选扩展或附录。

## Anchor Check

- **Original bottleneck**: 不可避免的 raw-vector verification 被实现成大量 candidate-local 小读，且旧 greedy 缺少固定模型下的最优性保证。
- **Why the revised method still addresses it**: 核心仍然是把同 tile、有序、mandatory vector reads 在硬 vector-byte amplification bound 下分段，并由 completion 复用自然覆盖的 inline payload。
- **Reviewer suggestions rejected as drift**: 没有加入 workload-aware 全局重排、learned latency model、跨查询 co-fetch 或新 SafeIn 分类器；这些会改变问题并超出两周期限。

## Simplicity Check

- **Dominant contribution after revision**: 一个 `span-reusable record substrate + amplification-bounded co-fetch execution` 的格式—执行合同。
- **Components removed or merged**: GE 不再称为 record-aware/layout-aware optimizer；exact 被并入主机制，承担可证明性而非独立性能贡献；SafeIn 从核心流程移到 conditional extension；独立 eager prefetch 从推荐方法删除。
- **Reviewer suggestions rejected as unnecessary complexity**: 不把 direct DP/Fenwick 选择扩展成大型算法设计空间；只补实际 run 分歧与 CPU 统计。不新增 auto-tuner 或 learned device cost。
- **Why the remaining mechanism is the smallest adequate route**: 格式、admission、planner、completion view 和 final missing fetch 正好闭合从“允许读哪些连续字节”到“这些字节如何被使用”的因果链，没有平行模块。

## Changes Made

### 1. 修正格式与 GE 的关系

- **Reviewer said**: GE 不读取 payload metadata，不能称为 record-aware exact planner。
- **Action**: 核心 planner 改称 `vector-only amplification-bounded exact planner`；系统贡献改称 `span-reusable record substrate with amplification-bounded exact co-fetch`。
- **Reasoning**: 格式影响 span bytes 的可复用价值，GE 决定 vector extents 的分组；它们在 completion/final materialization 处闭合，但不是同一个 bidirectional optimizer。
- **Impact**: 避免把 layout synergy 写成算法输入层的虚假耦合。

### 2. 收缩 exact novelty

- **Reviewer said**: endpoint dominance/Fenwick 是经典 DP 加速，GE 的端到端增量也小。
- **Action**: exact 定位为主机制的关键 enabling technique：在当前 fixed-order model 下消除 heuristic gap、提供可审计最优性；主要性能收益明确归因于 NoSpan→bounded span。
- **Reasoning**: 这与现有 NoSpan→GV 和 GV→GE 数量级一致。
- **Impact**: 算法论断由“新优化算法”降为“针对系统合同推导的 exact low-overhead planner”。

### 3. SafeIn 退出核心方法

- **Reviewer said**: 低 `rho` 只有 Pareto trade-off，证据不足以进入主贡献。
- **Action**: final default 冻结为 GE；SE 仅作为条件性扩展，完成 NoCombine zero-credit 与 consumption gate 后才允许在正文贡献末尾出现一句。
- **Reasoning**: `rho=0.1` 减请求但 QPS 小幅回退；eager SafeIn prefetch 在四个数据集均失败。
- **Impact**: 即使 SafeIn extension 最终失败，主线、方法和论文结构也不受影响。

### 4. 区分 amplification 语义

- **Reviewer said**: SE 的 utility-adjusted admission 不能称为同一个 vector-byte hard bound。
- **Action**: GE 报告 `A_vec=B/V` 并保证 `A_vec<=alpha`；SE 同时报告真实 `A_vec` 与 `A_eff=B/(V+rho S)`，只保证 `A_eff<=alpha`。
- **Reasoning**: credit 不会减少真实物理字节。
- **Impact**: 避免“1.5× 上限”在 SafeIn-aware 版本中的口径偷换。

### 5. 增加最小因果矩阵

- **Reviewer said**: 仅 Combined vs NoCombine 不能证明格式—span synergy。
- **Action**: 加入 `Combined/NoCombine × NoSpan/GE` 最小 factorial，并补 reusable/consumed bytes、final payload requests。
- **Reasoning**: interaction 才能回答 layout 是否让 span 放大字节产生额外价值。
- **Impact**: 一张小表替代一组发散的格式/算法 sweep。

## Revised Proposal

# Research Proposal: A Span-Reusable Record Substrate with Amplification-Bounded Exact Co-fetch

## Problem Anchor

- **Bottom-line problem**: 在 record-return 向量检索中，把不可避免的原始向量验证读取转化为受控、可复用的连续 co-fetch，在不改变候选 membership 与 exact verification 语义的前提下，减少小规模随机 I/O 请求；并据此形成一条可被实验直接支撑的论文创新主线。
- **Must-solve bottleneck**: 分离的 vector/payload 读取使系统在候选验证与结果物化阶段产生大量小读；现有 streaming greedy 虽能合并读取，却不保证在固定有序 run 和读放大约束下得到请求数最少、字节数次少的分组，也没有严格利用 span 已覆盖的、可能有用的 inline payload bytes。
- **Non-goals**: 本轮不重新设计 ANN、RaBitQ/SafeOut 或 SafeIn 分类器；不把固定顺序连续分段包装成 NP-hard 装箱；不设计跨查询、跨 tile 的全局布局优化；不声称单次大读在所有设备和负载上总是优于小读；不让 co-fetch 或 SafeIn 改变最终结果正确性。
- **Constraints**: 论文与补实验剩余时间不足两周；复用当前 `.clu` 地址元数据、packed/split layout、64 KiB tile、async/serial reader、现有数据集与结果；优先采用无需训练、可解释、确定性的算法；所有正文结论必须区分已验证的 GE/vector-only exact 与仍需因果消融确认的 SE/SafeIn-aware 扩展。
- **Success condition**: 主方法能被精确定义为一个固定有序 run 上的受约束分段问题；exact planner 与独立二次 oracle 一致并以可接受开销运行；NoSpan、greedy/exact、Combined/NoCombine 的结果分别支撑请求合并、算法最优性和格式—执行协同。SafeIn-aware 只有在 paired 端到端结果、credit/reuse 遥测和 NoCombine 零 credit 边界共同通过时才进入主文，否则降为可选扩展或附录。

## Technical Gap

在 RecordGate 先通过 bound-guided control 消除不必要的候选验证后，剩余 raw-vector reads 是 correctness path 上不可避免的物理工作。逐候选读取会产生大量小 I/O；简单相邻合并虽降低请求数，却既没有统一的读放大合同，也没有说明被放大的字节能否对 record return 产生价值。

相邻工作分别覆盖 SSD ANN I/O 合并、page co-location、异步 prefetch、large-value out-of-line 和 hybrid placement。因而论文不能把任一单点写成创新。缺失的系统合同是：

1. 格式让小 payload 位于 mandatory vector span 可能覆盖的物理路径上，同时让大 payload 外置以保护热路径；
2. planner 在相对 mandatory vector bytes 的硬放大上限内选择连续 reads；
3. completion 把真正覆盖的 inline payload 暴露为 reusable view；
4. exact top-k sealed 后只补读缺失 payload。

该闭环把“多读的字节”从纯 gap 变成可被核验和消费的潜在 record bytes，同时仍以真实物理字节约束 planner。

## Method Thesis

- **One-sentence thesis**: RecordGate combines a size-adaptive, span-reusable record substrate with amplification-bounded exact co-fetch, reducing requests for mandatory vector verification while reusing naturally covered inline payload bytes without changing exact-result semantics.
- **Smallest adequate intervention**: 不改变候选集合、顺序、bound 或 exact verification，只改变记录的 inline/external contract、mandatory reads 的分段和 completion view。
- **Modern primitive usage**: 这是确定性存储/I/O 规划问题；不使用 LLM、RL 或 learned cost。现代 leverage 来自 direct-address metadata、async completion views、混合对象放置和可审计 exact planning。

## Contribution Focus

- **Dominant contribution**: **A span-reusable record substrate with amplification-bounded co-fetch execution.** 小 payload 与 raw vector 共置，大 payload 外置；mandatory vector spans 在硬 `B/V` 上限内合并；已覆盖 inline payload 成为 query-local materialization view。
- **Key enabling technique, not a parallel contribution**: 针对 fixed ordered run 的 `O(n log n)` exact planner，在 `(requests,physical bytes)` 字典序目标下给出模型内最优计划，并保留 `O(n)` greedy fallback。
- **Optional supporting extension**: SafeIn-aware useful-byte credit；只有通过剩余因果 gate 才进入正文。
- **Explicit non-contributions**: inline/external placement、I/O coalescing、Fenwick DP optimization 单独均不主张新颖；不声称 latency-optimal、query-global optimal、跨 tile optimal 或设备普适最优。

## Proposed Method

### Complexity Budget

- **Frozen**: ANN/RaBitQ/SafeOut、exact frontier、candidate order、`.clu` direct addresses、tile/reader、final result semantics。
- **New**: span-reusable inline/external contract、completion payload view、GE exact planner、必要 telemetry。
- **Optional**: SE credit field/admission；不影响 GE 主线。
- **Excluded**: record reordering、learned device model、auto-tuned alpha、tail-aware DP、cross-query planning、eager SafeIn full-record prefetch。

### System Overview

```text
Candidates
  -> logical access control: Prune or Verify
  -> mandatory Verify candidates grouped by tile and physical order
  -> GE: exact vector-only bounded span partition
  -> issue contiguous span reads
       -> vector slices -> exact verification
       -> physically covered inline payload slices -> reusable views
  -> exact top-k sealed
  -> final missing-payload fetch

Optional only after validation:
resident SafeIn label + covered-inline extent -> discounted SE admission
```

### Span-Reusable Record Substrate

每个 cluster entry 提供 raw-vector offset、record extent 与 payload locator。小 payload 保持 inline，使它可能落在相邻 mandatory vector reads 的连续 extent 中；大 payload external，避免大对象扩大所有 vector reads。该格式的贡献不在“大小分层”本身，而在明确保证：completion 能从已读 span 中构造与 metadata 一致的 payload view。

可复用 bytes 必须满足：inline、物理覆盖、offset/length 可验证、buffer lifetime 足够、final materializer 可消费。padding、descriptor、未覆盖 endpoint tail、external/sidecar 和 metadata miss 均不计入 reuse。

### Vector-Byte Amplification Contract

对同一 tile 内按地址排序的 run，令 raw vector 起点为 `x_k`、长度为 `v`。区间 `i..j` 的物理 extent 和 mandatory bytes 为：

\[
B(i,j)=x_j+v-x_i,\qquad V(i,j)=v(j-i+1).
\]

GE 的 edge 仅在以下条件成立时可行：

\[
B(i,j)\le\alpha V(i,j).
\]

因此每个 GE span 都满足真实 vector-byte amplification：

\[
A_{vec}(i,j)=B(i,j)/V(i,j)\le\alpha.
\]

默认 `alpha=1.5` 只表示测试系统允许的 per-span 上限，不表示设备普适最优，也不约束后续 final payload fetch。

### Exact Fixed-Run Planner

把每个可行区间 `(i,j)` 视为 prefix DAG 中从 `i-1` 到 `j` 的边，成本为 `(1,B(i,j))`。目标按字典序最小化：

\[
\left(\#physical\ requests,\sum physical\ bytes\right).
\]

朴素最短路/DP 为 `O(n^2)`。vector-only admission 可改写为 start scalar 与 endpoint threshold 的一维 dominance，因此可对 scalar 坐标压缩，用 Fenwick suffix minimum 在 `O(n log n)` 时间、`O(n)` 空间求 exact 解。SafeIn-aware 扩展保持同一结构，但不是 GE 正文论断的前提。

这个 exactness 只覆盖 fixed run、fixed order、fixed tile、fixed edge contract 和上述字典序目标。主要端到端收益来自 bounded co-fetch 本身；exact planner 的角色是消除 greedy 的 heuristic gap，并让系统合同可证明、可审计、可复现。

实际实现对 `m<=8` 使用语义相同的 bounded direct DP fast path，较长 run 使用 endpoint-dominance/Fenwick。GV 保留为 `O(n)` 低 CPU fallback。选择 GE 不等于声称其 QPS 高于 GV，而是选择 theory-preferred、empirically non-inferior 的默认计划器。

### Completion-Time Reuse and Final Materialization

每个 span completion 按已验证 offsets 暴露 vector slices，并把真正落入 span buffer 的 inline payload slices登记为 query-local views。exact verification 仍是 membership 的唯一判据；reuse 只改变 payload 字节何时到达。top-k sealed 后，materializer 查询 view registry，只为缺失部分发起最终读取。

这一接口是格式与 span 的结合点：GE 不使用 payload size 决定边，但格式决定已选择 span 的附带字节能否成为实际有用数据。

### Conditional SafeIn-Aware Extension

该扩展不属于默认 GE。若 candidate 在 planner 前具有 SafeIn label，且 metadata 给出 span completion 可复用的 inline extent，定义：

\[
s_k=\mathbf1[SafeIn_k]\cdot inlineReusableBytes_k,
\qquad S(i,j)=\sum_{k=i}^{j-1}s_k.
\]

SE 使用：

\[
B(i,j)\le\alpha\bigl(V(i,j)+\rho S(i,j)\bigr).
\]

这不是与 GE 相同的 vector-byte hard bound。SE 只保证：

\[
A_{eff}=B/(V+\rho S)\le\alpha,
\]

同时必须单独报告真实 `A_{vec}=B/V`。`rho` 是保守 utility discount，不是命中概率；credit 不改变物理字节，也不保证 payload 最终被 top-k 消费。endpoint、external、sidecar 与 padding 为零 credit。独立 eager SafeIn full-record read 默认关闭。

### Failure Modes and Diagnostics

- **主要收益被误归因于 exact**: 分开报告 NoSpan→GV 和 GV→GE；前者证明 span，后者证明 exact 的增量。
- **格式与 span 只是相加而非协同**: 运行 `Combined/NoCombine × NoSpan/GE`，检验 interaction、reusable bytes 和 final payload requests。
- **SafeIn 只换来多读**: 失败即 `rho=0`，SE 降 appendix；不影响 GE。
- **`alpha` 口径偷换**: GE 报 `A_vec`；SE 同时报 `A_vec/A_eff`。
- **Fenwick 对短 run 不划算**: 保留 direct-DP fast path，报告 m 分布和 microbenchmark，不把算法选择包装成 novelty。
- **设备不偏好合并读**: 结论限定测试设备/负载，保留 NoSpan/GV fallback 与 alpha sensitivity。

## Novelty and Elegance Argument

本工作最可守的 novelty 是 candidate-to-record 路径上的格式—执行合同，而不是任何一个通用原语。相比仅合并向量 I/O，RecordGate把读放大中的 inline record bytes 变成可复用 payload view；相比仅做 hybrid placement，它用 mandatory verification span 定义 inline bytes 的执行价值；相比 workload-aware block layout，它不重排数据，也不声称 NP-hard；相比 page co-location/prefetch，它有真实 vector-byte amplification bound、exact fixed-run partition 和 final-missing fetch boundary。

endpoint-dominance solver 的论文价值在于：系统选择的简单 admission 恰好保留了一维可解结构，使 exact planning 不需要 `O(n^2)` 或 heuristic。应写作“derive an exact planner for this contract”，不应写成一般性算法突破。

## Paper-Facing Narrative

### Recommended core paragraph

> RecordGate treats post-ANN execution as two coupled decisions. Its logical controller first removes candidates that do not require exact verification under the configured confidence policy. It then organizes the remaining mandatory vector reads through a size-adaptive, span-reusable record substrate. Small payloads remain adjacent to raw vectors, while large payloads are reached through external locators. Within each ordered tile-local run, an exact planner coalesces vector reads under a hard byte-amplification bound. The returned span buffers provide both vector slices for exact verification and reusable views over any inline payload bytes they already cover; after the exact top-k is sealed, RecordGate fetches only the missing payload bytes.

### Recommended contribution bullets

1. **Bound-guided candidate-to-record access control.** We formulate the post-ANN boundary as an online access-control problem that uses bounded distance evidence and the evolving exact frontier to avoid unnecessary raw-vector verification without making prefetch part of correctness.
2. **A span-reusable record substrate with amplification-bounded exact co-fetch.** We co-locate small payloads with raw vectors, keep large payloads external, and derive an exact `O(n log n)` planner that minimizes requests and then bytes for fixed ordered runs under a hard vector-byte amplification bound. Span completions expose covered inline payload as reusable materialization views.

不要把 evaluation、prefix-resident bits 或 SafeIn 单独列为新的 headline contribution。prefix-resident 可作为 deployment knob；SafeIn 若通过 gate，只在第二条末尾增加一句：

> We further show that the same formulation can conservatively credit physically covered inline bytes from high-confidence candidates, providing an optional request/byte Pareto point without a separate eager-prefetch path.

### Claims that must not appear

- “Large reads are always faster than small random reads.”
- “The span problem is NP-hard / bin packing.”
- “GE is a record-aware optimizer.”
- “Exact is faster than greedy.”
- “All physical reads are bounded by 1.5×.”
- “SafeIn prefetch improves all workloads.”
- “NoCombine proves superiority over FlatStor/Lance.”

## Claim-Driven Validation Sketch

### Claim 1: bounded co-fetch reduces request overhead; exact removes the heuristic gap at low planning cost

- **Minimal experiment**: NoSpan, GV, GE on ESCI/MSMARCO representative `nprobe={96,192}` with paired repeats；oracle differential tests；m/run gap telemetry。
- **Metrics**: requests, bytes, `A_vec`, QPS, p99, planner ms, recall/probed/reranked, fallback；greedy/exact different-plan ratio and request/byte gap distribution。
- **Existing evidence**: NoSpan→GV requests `-53%~-68%`、QPS `+0.91%~+4.81%`；GE-GV core QPS within `-0.62%~-0.89%`、requests non-increasing；planner约 `0.06 ms/query`；oracle一致、43/43 tests。
- **Claim boundary**: span 承担主要性能提升；exact 承担最优性与小幅 request improvement。

### Claim 2: layout makes naturally amplified bytes reusable for record materialization

- **Minimal experiment**: `Combined/NoCombine × NoSpan/GE` 两因素矩阵，优先 ESCI/MSMARCO，再用 COCO/Vox 做正向/边界点。
- **Metrics**: vector requests/bytes、payload requests/bytes、span-covered reusable bytes、actually consumed bytes、final missing-payload reads、QPS/p99、recall。
- **Decisive evidence**: GE 在两种格式均可减 vector requests；只有 Combined 产生合法 inline views，并相对其 NoSpan cell 进一步减少 final materialization work。interaction 方向与机制一致。
- **Reuse boundary**: 现有 GE Combined vs NoCombine QPS 正向点可复用为 drift anchors，但不能单独完成 synergy claim。

### Conditional Claim 3: SafeIn credit provides an optional request/byte Pareto point

- **Minimal experiment**: `SE(rho=0.1) vs GE` 在 Combined/NoCombine 下 paired，核对 zero-credit 与 consumed-credit。
- **Inclusion gate**: 至少两个机制命中数据集 requests 稳定下降，QPS/p99/bytes 过门禁；Combined 有 credit/reuse，NoCombine 为零；正文明确其为 Pareto extension 而非速度贡献。
- **Current evidence**: ESCI/MSM requests约 `-0.69%/-0.22%`、QPS约 `-0.40%/-0.27%`；尚不足以进入主贡献。

## Experiment Handoff Inputs

- **Must prove**: NoSpan→span 主收益；GE model exactness；format×span reuse interaction。
- **Must run now**: 一张两因素 representative 表与 exact plan-gap telemetry；最多再做一次 SE NoCombine gate。
- **Reuse**: existing NoSpan anchors、GE/GV paired results、oracle/CTest/CPU logs、GE Combined/NoCombine drift anchors、旧完整 NoCombine sweep。
- **Do not rerun**: 大型 alpha/rho grid、独立 eager SafeIn prefetch、全量四方案 sweep、跨设备大矩阵，除非核心因果表失败。
- **Highest risk**: format×span interaction 可能主要体现在 final payload work 而非 QPS；必须把机制指标与端到端指标分开主张。

## Compute & Timeline Estimate

- 训练/GPU：0。
- exact gap telemetry：0.5 天。
- `Combined/NoCombine × NoSpan/GE` representative paired：1--2 天。
- 可选 SE NoCombine gate：0.5--1 天；失败即停止。
- narrative/method/claim ledger 更新：1--2 天。
- 其余时间留给 final binary main anchors、并发/cold-cache 或写作，不再扩张方法。
