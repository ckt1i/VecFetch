# Round 1 独立研究评审（ICDE/PVLDB/系统方向）

## 一、总体判断

我支持把 SafeIn 从论文的主方法、默认执行路径和正式主结果中移除。

这不是因为 SafeIn 在理论上“绝对不可能有效”，而是因为：

1. SafeIn 只是当前已见集合上的 speculative admission，不是对最终 membership 的单调证书；
2. 最新四组相互独立、控制更严格的实验——eager full-record、cold-prefix、SE/GE、tile/read-bound sweep——都没有显示稳定收益；
3. SafeIn 引入了 label、credit、tail、confidence、wasted bytes 等一整套额外概念，却只能带来请求数和字节数之间非常弱的 Pareto 移动；
4. 它会显著模糊真正成立的机制：SafeOut、bounded span、natural payload reuse 和异步执行；
5. 剩余时间不足两周，继续为 SafeIn 寻找局部胜点的论文收益远小于补齐现有主线的因果控制。

但需要严格区分三种“移除”：

- **Paper removal：应立即执行。** SafeIn 不进入 method overview、贡献列表、主算法、默认配置或 headline result。
- **Default-off：应立即执行。** eager record prefetch、cold-prefix prefetch、SafeIn-aware span credit 和 tail extension 在论文冻结配置中全部关闭。
- **Physical code deletion：暂不建议立即执行。** 两周内删成熟路径可能引入回归。先把功能隐藏在非默认实验 flag 后，保证默认二进制的 telemetry 明确为 `safein_credit=0, eager_prefetch=0, tail=0`。论文 artifact 冻结后，再单独清理 inactive code。

SafeIn 的负结果可以保留为 appendix 或 design alternatives 中的一小段，用来解释为什么系统采用 one-sided confidence control。不要把它放进主性能表，也不要花一整节介绍一个最终不用的机制。

移除 SafeIn 后，论文仍然可以成立，而且叙事会更干净。但当前证据还不足以直接投稿：固定 rerank baseline、pipeline overlap 的因果控制，以及 layout–span interaction 至少还需补齐。

---

## 二、SafeOut 与 SafeIn 的理论论证

### 2.1 SafeOut 的单调证书基本成立，但必须补齐四个限定

设当前已经处理的候选集合为 $C_t$，每个候选的真实距离为 $d_i$，区间为 $[L_i,U_i]$。令

$$
T_t=\text{kth-smallest}\{U_j:j\in C_t\}.
$$

如果至少已有 $k$ 个候选，并且

$$
L_i>T_t,
$$

那么在联合区间事件

$$
\mathcal E=\bigcap_j\{L_j\le d_j\le U_j\}
$$

成立时，当前集合中至少有 $k$ 个候选满足

$$
d_j\le U_j\le T_t<L_i\le d_i.
$$

所以 $i$ 不可能进入最终 top-k。后续候选只会增加竞争，不会使它重新进入 top-k。同时，随着候选加入，k-th order statistic $T_t$ 不会上升，因此该证书是单调的。

这个论证是严谨的，但论文必须限定：

1. **只对 ANN/IVF 已生成的候选流成立。** 它不修复 candidate generation 阶段遗漏的真近邻，也不能单独保证全库 exact top-k。
2. **依赖联合置信事件。** 若 RaBitQ 只提供每候选 marginal coverage $1-\delta$，不能直接写 query-level failure probability 也是 $\delta$。需要 query-level simultaneous calibration、union-budget，或明确写 $P(\mathcal E^c)\le\sum_i\delta_i$。
3. **必须使用严格不等式和清楚的 tie policy。** `L_i > T_t` 比 `>=` 安全；距离相等时需要定义稳定 tie-breaking。
4. **自适应扫描不会破坏条件于 $\mathcal E$ 的确定性证明，但置信度口径必须覆盖所有被自适应查询的区间。** 普通单点置信区间不自动等于 anytime-valid guarantee。

因此推荐表述是：

> Conditional on the configured query-level bound event, once a candidate is pruned by the lower-bound test, the evolving frontier can only make that certificate stronger.

不能写：

- “SafeOut 绝不会误判”；
- “SafeOut 保证全局 exact top-k”；
- “只要单候选 bound 是 $1-\delta$，整个查询就是 $1-\delta$ 正确”。

### 2.2 “SafeOut 只会多读 Uncertain，不会误读”也需要修正

SafeOut certificate 一旦成立确实不会被未来候选撤销。但是一个当前仍为 Uncertain 的候选，可能随着 frontier 收紧在稍后变成 SafeOut。

因此，如果 pipeline 在 IVF probing 期间立即发起其 raw-vector read，系统可能读取一个“稍后本可被剪掉”的候选。这是额外 work，不是 accuracy error。

应区分：

- pruning certificate 的单调性；
- Uncertain 状态的暂时性；
- 早发 I/O 带来的 latency–work tradeoff。

如果这部分没有 telemetry，论文只能写“asynchronously issues currently admitted verification reads”，不能声称所有提前发出的向量读取最终都是必要读取。

### 2.3 SafeIn 的 non-monotonicity 判断正确，但不能夸大为“必然不准确”

若 SafeIn 依据当前已见集合的 lower frontier $S_t$，例如 $U_i<S_t$，随着更好的候选出现，k-th lower statistic 可能下降，所以早期条件可能失效。更严格地说，若要证明 $i$ 属于当前集合 top-k，应使用其他候选的 lower bounds，并处理好是否将 $i$ 自身包含在 order statistic 中的 off-by-one 问题。

更关键的是，当前未见候选没有 lower bound witness。因此 early SafeIn 最多说明“在当前已见集合中很强”，不能说明它一定属于最终 candidate-set top-k。

但以下说法仍然过强：

- “阈值收紧必然导致所有早期 SafeIn 被移除”；
- “SafeIn 理论上无效”；
- “SafeIn 不准确，所以不可能带来性能收益”。

正确结论是：

> SafeIn lacks a scan-monotone final-membership certificate and therefore remains speculative. Such speculation could still be useful if precision and hidden-I/O benefit were high, but the controlled experiments show that they are not high enough in the evaluated workloads.

也就是说，**理论解释为什么 SafeIn 风险更高；实验决定为什么现在应该删除。**

---

## 三、对拟议三点叙事的逐条审查

### 3.1 “传统固定 rerank 阈值没有理论依据且误差大”

这是当前最危险的表述。

固定 rerank budget 确实缺少 query-level adaptation，但不能笼统说它“没有理论依据”。已有系统可能通过经验调参、validation recall、rerank factor 或 cost model 选择预算。它也未必“误差大”；预算足够大时 recall 可以很高，只是会多读。

当前 NoSafeOut 实验比较的是 RecordGate dynamic pruning 与几乎验证全部剩余候选。它不是常规 fixed-depth rerank baseline，因此不能证明 RecordGate 优于“传统固定 budget”。

建议改为：

> Static rerank budgets apply the same verification effort across queries despite substantial variation in estimator uncertainty and frontier evolution. RecordGate instead makes candidate-level verification decisions from bounded distance evidence and the evolving top-k frontier.

若要进一步写“在 matched recall 下减少 verification I/O”，必须补 fixed budget curve。

不要写“剪枝与 prefetch 相结合”。SafeIn 移除后，SafeOut 是 pruning，raw-vector reads 是 verification scheduling。把 correctness-path mandatory reads 称为 prefetch 会重新制造概念混乱。

### 3.2 Pipeline、span DP 与 exact $O(n\log n)$

这部分可以成立，但现在混合了三个不同层次：

1. 何时发请求：跨 IVF probing 的异步调度；
2. 如何组织请求：amplification-bounded span；
3. 如何求 span partition：exact endpoint-dominance DP。

应写成一个 physical execution contribution，而不是三个独立创新。

当前 Full vs NewNoPipeline 同时改变 overlap、batching、reuse、tail 和 SafeIn，不能证明性能差来自 CPU/I/O overlap。COCO 在无 SafeIn/reuse/span 命中时仍有收益，只能说明 async/batching 路径本身有价值，不能单独定位为跨-probe overlap。

关于 exact planner：

- 可以声称在冻结的 fixed-order、tile-local、fixed-admission、lexicographic objective 下 exact；
- 可以声称独立二次 oracle 验证；
- 不可以把 endpoint dominance + Fenwick 包装成独立通用算法突破；
- 不可以说 exact 比 greedy 更快；
- 不可以暗示 $O(n^2)$ 在当前 trace 中是不可承受的，因为实际 run 很短；
- GE 相比 GV 的 QPS 是负的，算法的意义是可审计最优性和小幅 request reduction，不是 headline speedup。

最安全的包装是：

> We formulate tile-local read coalescing as an ordered interval-partition problem and derive an exact $O(n\log n)$ solver for the frozen request-first objective. The solver provides model-level optimality at small measured planning cost; the main end-to-end gain comes from bounded coalescing itself.

还必须明确 planner 的“最优”只针对本轮已经 admitted 的 read set。它不是对动态 frontier、未来 pruning 和全查询执行的 global optimum。

“提前读取可能的 top-k 数据”也不准确。自然 span reuse 并不知道哪些候选最终进入 top-k。应写：

> the mandatory vector span may naturally cover inline payload bytes of candidate records, which are registered as reusable views and consumed only if the corresponding candidates survive into the final top-k.

### 3.3 自适应 raw-vector/payload layout

这一点有系统价值，但“把小 payload 和 vector 放一起，大 payload 外置”本身很容易被 reviewer 视为已有 hybrid value placement、late materialization 或 inline/out-of-line storage 的组合。

创新不能落在 size threshold 本身，而应落在完整的执行合同：

- logical controller 产生 verification set；
- layout 暴露可 span-address 的 inline extent；
- bounded span 限制多读字节；
- completion 建立合法 payload view；
- exact top-k sealed 后只读取缺失 payload；
- external payload 保持大对象不污染 vector hot path。

因此不要把 format 单独列为第三条 headline contribution。它应该和 span planning、completion-time reuse 合并为一个“span-reusable record substrate”。

“payload slice”还需要在论文中说明 prefix 与 external suffix 的 locator/offset、是否重复存储、更新和一致性语义、view lifetime、部分覆盖如何 completion、padding/descriptor 是否计入 physical bytes，以及 final materializer 如何判断还缺哪些 bytes。否则 reviewer 会认为这只是把两份文件拼起来。

---

## 四、移除 SafeIn 后贡献是否足够

我建议只保留两条 contribution。

### Contribution 1：逻辑访问控制

> A confidence-budgeted candidate-to-record access controller that uses bounded distance evidence and an evolving exact frontier to adapt verification effort at candidate granularity.

这条贡献的核心不是“动态阈值”四个字，而是 one-sided, monotone pruning certificate、query/candidate-level adaptive verification、与 candidate generation recall 分离的 correctness contract，以及对 record I/O 的直接控制。

### Contribution 2：物理格式—执行协同

> A span-reusable record substrate with amplification-bounded co-fetch, combining size-adaptive inline/external placement, asynchronous read scheduling, exact fixed-run partitioning, and completion-time payload reuse.

其中 bounded span 是主要性能机制；exact $O(n\log n)$ planner 是 enabling technique；adaptive layout 是 reuse substrate；async pipeline 是 execution mechanism；natural payload reuse 是结果，不是 SafeIn prefetch。

不建议列第三条 format contribution，也不建议把 evaluation 当 contribution。

只有当 `NoOverlapAsyncFinal` 证明跨-probe overlap 具有独立、稳定且较大的收益，并且设计本身有足够具体的 scheduling contract 时，才可以考虑把 execution pipeline 拆成第三条。以当前证据，不足。

---

## 五、已有证据可复用范围

| 已有结果 | 可以支持 | 不可以支持 |
|---|---|---|
| NoSafeOut 慢 37.3×/65.8×，工作量高两个数量级 | bound-guided pruning 对避免验证全部候选非常重要 | 优于传统 fixed rerank budget；固定 budget 误差大；无条件正确 |
| eager SafeIn 20/20 negative | eager speculative record prefetch 不适合作为默认机制 | SafeIn 在所有设备和 workload 永远无用 |
| cold-prefix 与 SE/GE 无稳定胜点 | 删除 SafeIn credit/prefix/tail 的实证合理性 | natural span payload reuse 无效 |
| GEReuse vs NoPayloadReuse | ESCI/MSM final materialization requests/bytes 减少 | 四数据集普遍 QPS 加速；COCO/Vox 同样受益 |
| NoSpan→GV | bounded span 显著减少 requests，且 ESCI/MSM QPS 改善 | exact planner 本身带来这些收益 |
| GE→GV + oracle + tests | GE 在冻结模型下 exact，并进一步减少少量 requests | GE 比 GV 更快；通用 I/O optimum |
| Full vs NewNoPipeline | 完整异步执行 bundle 有收益 | 全部收益来自 CPU/I/O overlap |
| 旧 Vox/MSM P0/P1 正结果 | 可作为历史 drift/debug anchor | SafeIn 的独立正证据 |
| recall/probed/reranked 一致 | 测试配置下语义一致 | 概率 bound 的全局形式保证 |

---

## 六、最小 results-to-claims matrix

| 实验结果 | 允许的 claim | 必须降级/删除的 claim |
|---|---|---|
| fixed-budget curve 中 RecordGate 在 matched recall 下稳定少读 | query-adaptive bounded verification 优于 static effort allocation | — |
| 只有 NoSafeOut 显著变慢，未跑 fixed budget | pruning 避免 verify-all 成本 | 不能批评传统 fixed budget |
| `NoOverlapAsyncFinal` 明显慢于 Full | 跨-probe issuance 的 CPU/I/O overlap 有独立收益 | — |
| 两者无显著差异 | async/batching path 可行 | 删除 overlap speedup claim |
| `Combined/NoCombine × NoSpan/GE` 显示只有 Combined+GE 减少 final payload work | layout–span interaction 和 natural payload reuse 成立 | — |
| GE 只减少 vector requests，没有减少 final payload work | bounded span 成立 | layout-enabled record co-fetch 主张降级 |
| GE 与 oracle 一致，且 lexicographic objective 优于/等于 GV | 模型内 exact optimality | 不能写端到端 latency optimal |
| GE QPS 低于 GV | exact 是审计性/请求 Pareto 选项 | 不能把 exact 作为主要加速来源 |
| query-level joint bound 校准成立 | confidence-budgeted pruning guarantee | — |
| 只有 marginal interval/empirical recall | 测试中 recall 稳定 | 删除 query-level failure guarantee |
| SafeIn 各控制实验继续为负或不稳定 | default-off，并作为 rejected speculative design | 不再写 SafeIn extension |
| SafeIn 个别点为正但跨数据集/重复不稳定 | workload-specific appendix observation | 不进入主方法或默认配置 |

---

## 七、推荐论文结构

1. **Introduction**：id boundary 导致 candidate evidence、verification I/O、record materialization 脱节；两个核心问题是哪些候选需要 exact verification，以及不可避免的 reads 如何执行；只列两条 contribution。
2. **Background and Motivation**：ANN candidate generation 与 record-return 路径；static rerank effort 的 query variability；separated vector/payload access 的 request amplification；不做“大读普遍更优”的陈述。
3. **Correctness and Access-Control Contract**：candidate-set scope、RaBitQ intervals、query-level confidence event、monotone SafeOut certificate，以及 candidate-generation recall 与 post-ANN verification guarantee 分离。
4. **Bound-Guided Verification**：evolving frontier、candidate states、何时发出读取，以及 early issuance 的 latency–work tradeoff。
5. **Span-Reusable Physical Execution**：adaptive layout、ordered runs、amplification contract、partition objective、exact solver、completion views、final missing-payload fetch 和 async pipeline。
6. **Implementation**：io_uring path、buffer lifetime、metadata、cache policy、fallback；默认 SafeIn 路径关闭。
7. **Evaluation**：end-to-end、fixed budget、SafeOut、NoSpan/GV/GE、layout/reuse、overlap 和 sensitivity。
8. **Discussion and Limitations**：probabilistic failure、ANN recall、设备依赖、SafeIn negative design。
9. **Related Work / Conclusion**。

---

## 八、Mock ICDE/PVLDB Review

### Summary

本文提出 RecordGate，一个面向 record-return 向量检索的 candidate-to-record 协同执行框架。系统首先利用量化距离区间与动态 top-k frontier 减少不必要的原始向量验证；随后将剩余候选的向量读取组织为带字节放大约束的连续 span，并通过 size-adaptive inline/external record layout 复用 span 自然覆盖的 payload bytes。作者还将固定有序 run 的读取合并建模为区间分段问题，并给出一个 $O(n\log n)$ exact solver。此前基于 SafeIn 的 speculative payload prefetch 在更严格实验中没有稳定收益，最新设计将其从默认路径移除。

### Strengths

- 逻辑访问控制与物理 record materialization 之间的连接是一个真实、重要且容易被分层系统忽略的问题。
- SafeOut 的 one-sided monotone certificate 比正向 membership prediction 更适合作为 correctness-sensitive I/O gate。
- NoSafeOut 的工作量和延迟差距很大，说明 verification reduction 确实是关键瓶颈。
- bounded span 显著减少请求，且在部分数据集带来实际 QPS 改善。
- exact planner 与独立 oracle 对齐，方法语义相对清晰。
- 作者对 SafeIn 负结果和旧实验混杂的处理诚实，删除无效组件提高了整体可信度。
- natural payload reuse 不改变 membership correctness，设计边界合理。

### Weaknesses

1. 当前没有公平的 static rerank budget baseline，核心 motivation 中对传统 rerank 的批评尚未被实验支持。
2. pipeline ablation 混合了多种因素，CPU/I/O overlap 的独立贡献没有建立。
3. exact planner 相对 greedy 的端到端收益很小甚至为负，且实际 run 很短；其算法贡献可能被认为是标准 dominance DP 优化。
4. adaptive inline/external layout 与 I/O coalescing 都有大量相关工作，论文必须更明确地证明 candidate-to-record interaction，而不是把已知技术简单组合。
5. payload reuse 当前只在 ESCI/MSM 产生机制收益，跨 workload 普适性有限。
6. 概率区间的 query-level failure accounting 尚未给出，现有表述容易把 marginal empirical bound 误写成整体正确性保证。
7. 早发 raw-vector reads 可能读取后来会被 frontier 剪掉的候选，这一 latency–work tradeoff 尚未量化。
8. 结果对 cache conditioning、NVMe 状态和执行顺序较敏感，正式统计协议需要冻结。

### Questions for Authors

1. SafeOut 的置信预算是 per candidate、per query，还是 across-query？自适应候选数下如何控制整体失败概率？
2. RecordGate 的 correctness 是相对 IVF candidate pool，还是声称相对全库 exact top-k？
3. 与固定 rerank depth 在 matched recall 和 matched bytes 下相比，RecordGate 的真实优势是多少？
4. 有多少提前发出的 Uncertain vector reads 在完成前或完成后变成可 SafeOut？
5. 在相同 io_uring batching 和 layout 下，仅关闭跨-probe issuance 会损失多少性能？
6. Combined layout 与 GE 是否存在可统计验证的 interaction，而不仅是两个各自独立的效果？
7. 实际 run 最大很小时，为什么需要 $O(n\log n)$ solver，而不是 bounded quadratic DP？该选择的系统收益是什么？
8. payload prefix 的一致性、更新和 duplicated-storage 成本如何处理？
9. 在不同 SSD、queue depth 或 read-size regime 下，1.5× amplification 的选择是否仍合理？

### Score

**5/10：Weak Reject / Borderline**

### Confidence

**4/5**

### Verdict

方法核心具备成为 ICDE/PVLDB 论文的潜力，移除 SafeIn 是正确方向；但当前稿件的三项中心因果链——相对 fixed budget 的优势、跨-probe overlap、layout–span interaction——至少有两项尚未由受控实验建立。若补齐 P0 实验并收紧理论表述，预期可以提升到 **7/10 weak accept**。继续优化 SafeIn 不会显著提高接受概率。

---

## 九、最多八项 actionable items

### P0

1. **冻结 SafeIn-free paper configuration。** 主路径设置 `credit=0, eager=0, cold-prefix=0, tail=0`；保留 natural span reuse；输出一次配置和 telemetry 审计，物理删代码延后。
2. **补 fixed rerank budget Pareto curve。** 在 ESCI/MSMARCO 上选择 3–5 个覆盖当前 dynamic reranked-count 分布的 budget，比较 matched recall 下的 requests、bytes、QPS、p99；NoSafeOut 只保留为 verify-all 上界。
3. **补 `NoOverlapAsyncFinal`。** 保持相同 io_uring、batch size、span、layout 和 payload reuse，只把 issuance 延后到 probing 完成后；否则从论文删除“CPU/I/O overlap 带来性能提升”的因果表述。
4. **补 `Combined/NoCombine × NoSpan/GE` 最小两因素矩阵。** 至少 ESCI/MSM，报告 vector/payload requests、physical bytes、covered reusable bytes、actually consumed bytes、final missing-payload reads 和 interaction。
5. **冻结概率 correctness contract。** 明确 candidate-set scope、联合置信事件、query-level failure budget、strict inequality 和 tie policy；若没有 simultaneous guarantee，则改成 empirical confidence policy，不写无条件安全。

### P1

6. **增加 early-issued read waste telemetry。** 统计 issuance 后后来变成 SafeOut 的 candidate/read bytes，给出 pipeline latency–work tradeoff。
7. **收缩 exact planner claim 并补 plan-gap 分布。** 报告 run-size、GV/GE 不同计划比例、request/byte gap、planner time；保留 oracle exactness，不把 $O(n\log n)$ 作为独立算法突破。

### P2

8. **统一正式统计与缓存协议。** 固定 warmup、cache conditioning、A/B 顺序交替和至少五次 paired repetitions；SafeIn 负结果只放 appendix/design alternative，不再做新 sweep。
