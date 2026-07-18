# Agent B 独立研究评审：RaBitQ 校准区间与 raw-vector I/O admission

> 日期：2026-07-18  
> 评审角色：ICDE/PVLDB 系统论文独立 reviewer B  
> 评审范围：仅审查 SafeIn 移除后，RecordGate 能否把“RaBitQ 误差估计 → 候选距离区间 → query-time frontier → raw-vector I/O 控制”作为论文贡献。  
> 外部 reviewer 状态：`research-review` 技能要求调用 Codex MCP 的 `gpt-5.4`、`xhigh`。本会话没有暴露 `mcp__codex__codex` / `mcp__codex__codex-reply`，因此无法建立外部 threadId。以下由独立 Agent B 以同等 xHigh 深度完成 Round 1、针对性反驳与 Round 2 收敛；没有借用主线程的结论。

## 0. 最终结论先行

1. **不能把“从 RaBitQ 理论误差界得到逐候选距离区间，并据此决定 rerank”本身写成 RecordGate 的算法创新。** RaBitQ SIGMOD 2024 已经给出概率距离误差界，并明确使用候选距离下界与当前精确近邻距离比较来决定是否 rerank；原论文甚至明确说“bound-based reranking 的思想不是新的”。

2. **现有实现也不能严谨地称为直接采用 RaBitQ 理论界。** RecordGate 的生产 Stage 1 使用 FastScan sign estimator 与量化 query；它没有直接实现原始 1-bit RaBitQ 论文中的无偏校正形式。系统通过数据采样得到归一化绝对距离误差的经验 P95/P99 分位数，再传播成候选半径。因此当前更准确的名称是：

   > implementation-calibrated RaBitQ-derived distance intervals

   而不是：

   > RaBitQ theoretical confidence intervals

3. **可构成论文增量的是完整的 storage-facing control loop，而不是误差界本身：**

   - 对实际 SIMD estimator 的残差进行离线校准；
   - 用候选残差范数和 query residual norm 把全局校准尺度转成候选相关区间；
   - 维护当前已见候选的第 `k` 小 upper-bound frontier；
   - 只对满足 `L_i <= T_t` 的候选发起 raw-vector I/O；
   - frontier 随 probe 单调收紧，把估计证据转化为存储访问控制。

   这可以作为 RecordGate 的一个**重要机制子贡献**，并与 span/layout/pipeline 共同组成系统贡献；单独作为“新的 RaBitQ 理论”则创新度不足。

4. **当前最大正确性缺口不是公式，而是概率合同。** P99 是采样 pair 的经验覆盖率，不是一个 query 中所有候选区间同时成立的概率，也没有有限样本校正。若一条 query 检查大量候选，简单的 per-pair P99 不能推出 query-level 99% safety。论文当前不能写 `query-level conservative guarantee`、`safe certificate` 或 `no false pruning`。

5. **还有一个必须在投稿前关闭的 Stage 2 缩放规律验证缺口。** 正式实验使用 `legacy_per_cluster`，该路径最终调用 `CalibrateFastScanPerClusterEpsilon`，所以正式 P99 校准的是 **Stage 1 FastScan**，并不存在“先直接校准 Stage 2、随后又除以 8”的已确认 double scaling。运行时在 `official_1_plus_n,total_bits=4` 的 Stage 2 中把该 Stage 1 radius 除以 `2^(4-1)=8`；这一缩放符合旧设计意图，但当前 formal calibration 本身没有直接验证 Stage 2 coverage。因此仍应做 fresh Stage 1/Stage 2 violation audit，不过风险应准确描述为“未经独立覆盖验证的缩放规律”，而不是“校准语义冲突”。

6. **推荐定位：有条件接受为“calibrated interval-to-I/O control”，不接受为“新的 RaBitQ error-bound method”。** 当前独立创新强度约 `4/10`；若补齐 query-level calibration、stage consistency、fixed-R matched-recall 和 frontier ablation，可提升到 `6.5–7/10`，足以成为完整系统论文的一条支柱，但仍应与物理布局和 span execution 联合呈现。

---

## 1. 审查材料与边界

### 1.1 代码与规格

- `src/index/ivf_builder.cpp:544-678`：构建期 FastScan epsilon 校准。
- `benchmarks/rabitq_bench_calibration.cpp:38-229,570-699`：benchmark runtime 的 split epsilon、legacy-per-cluster/global-pair 校准。
- `src/rabitq/rabitq_estimator.cpp:341-405,410-493`：Stage 1 FastScan、1-bit estimator、official 1+n Stage 2 estimator。
- `src/query/overlap_scheduler.cpp:823-874,3557-3610`：upper-bound heap、frontier 与 query residual margin factor。
- `src/index/cluster_prober.cpp:217-297,724-791`：候选级 margin、Stage 2 margin 缩放和 SafeOut 判定。
- `include/vdb/query/overlap_scheduler.h:348-379`：frontier heap 的实现合同。
- `tests/query/overlap_scheduler_test.cpp:173-191,1352-1363`：第 `k` 小 upper bound 与 heap 未满行为测试。
- `openspec/changes/archive/2026-06-03-safe-boundary-error-frontier/design.md`：区间语义的设计演化。
- `openspec/changes/archive/2026-06-03-safe-boundary-error-frontier/validation.md:120-143`：旧版 false SafeOut 观测。
- 正式 P0/P1 结果中的 `results.json`：P99 epsilon cache、official 1+n、4-bit 口径。

### 1.2 文献基线

- [RaBitQ, SIGMOD 2024](https://arxiv.org/html/2405.12497)：无偏 estimator、概率误差界和 error-bound-based reranking。
- [RaBitQ Library, VecDB 2025](https://openreview.net/forum?id=OeZHhOsFir)：重述 RaBitQ 理论并提供与 ANN index 集成的实现技术。
- [FusionANNS, FAST 2025](https://www.usenix.org/conference/fast25/presentation/tian-bing)：SSD raw-vector rerank、query-dependent rerank 数量、布局与 I/O 去重，说明“减少 rerank I/O”本身不是空白。

本报告不代替单独的 `novelty-check` 全量检索；它只给出足以约束论文措辞的最近邻证据。

---

## 2. 当前实现究竟怎样估计和计算误差界

## 2.1 距离估计的代数

对 cluster centroid `c`，定义：

\[
r_i = \lVert o_i-c\rVert_2,\qquad
r_q = \lVert q-c\rVert_2,
\]

归一化 residual 的真实内积为 `s_i`，sketch estimator 为 `\hat s_i`。平方距离满足：

\[
d_i = r_i^2+r_q^2-2r_i r_q s_i,
\]

\[
\hat d_i = r_i^2+r_q^2-2r_i r_q \hat s_i.
\]

所以：

\[
|\hat d_i-d_i|=2r_i r_q|\hat s_i-s_i|.
\]

若有一个内积误差尺度 `epsilon`，候选距离半径可写为：

\[
e_i=2r_i r_q\epsilon.
\]

对应区间为：

\[
L_i=\hat d_i-e_i,\qquad U_i=\hat d_i+e_i.
\]

这一步是直接的误差传播，不是新的数学定理。

## 2.2 构建期 epsilon 实际是经验分位数

`src/index/ivf_builder.cpp:544-678` 的 `CalibrateEpsilonIp` 做的是：

1. 每个 cluster 最多选择 `epsilon_samples` 个成员作为 pseudo-query；默认值在 `include/vdb/index/ivf_builder.h:92-97` 中为 20。
2. pseudo-query 与同 cluster 的其他成员组成 pair。
3. 用生产 FastScan Stage 1 kernel 得到 `dist_fastscan`。
4. 计算 exact L2 `dist_true`。
5. 只保留 `dist_true ∈ [0.1d_k,10d_k]` 的 pair。
6. 收集：

\[
z_{q,i}=\frac{|\hat d_{q,i}-d_{q,i}|}{2r_i r_q}.
\]

7. 排序并取经验 `percentile`；构建默认是 P95。

因此，metadata 中的 `epsilon` 实际是：

\[
\hat\epsilon_p=\operatorname{EmpiricalQuantile}_p\{z_{q,i}\}.
\]

它不是：

- 对每个候选独立采样得到的 epsilon；
- per-query epsilon；
- rigorous maximum error；
- 自动继承 RaBitQ theorem failure probability 的参数。

更准确地说：epsilon 是全局的，而最终的 `e_i` 因 `r_i` 和 `r_q` 不同而成为 candidate-dependent radius。

## 2.3 runtime benchmark 使用另一条校准路径

正式实验通常传入：

```text
--safeout-epsilon-percentile 0.99
--epsilon-samples 100
--epsilon-sampling-mode legacy_per_cluster
--rabitq-validation-mode official_1_plus_n
```

`benchmarks/rabitq_bench_calibration.cpp:591-699` 会：

- 仍然收集相同形式的 normalized absolute distance error；
- `legacy_per_cluster` 时，函数在末尾调用 `CalibrateFastScanPerClusterEpsilon`：每 cluster 采样 query、与 cluster members 配对，并且**直接执行 Stage 1 `EstimateDistanceFastScan`**；
- `global_pair` 时把 `epsilon_samples` 解释为总 pair 数，并由 `record_error` 调用 `EstimateRabitqCalibrationDistance`；只有该分支在 official 1+n 配置下才选择 official Stage 2 estimator；
- 正式脚本使用 `legacy_per_cluster`，所以 formal P99 是 **Stage 1 FastScan normalized-error quantile**。`results.json` 中的 `rabitq_estimator_mode=official_1_plus_n` 描述索引/Stage 2 格式，不能据此把 formal epsilon 误读为 Stage 2 直接校准结果。

正式 P0 结果显示：

| dataset | loaded epsilon | runtime P99 epsilon | mode | estimator |
|---|---:|---:|---|---|
| Amazon ESCI | 0.056730 | 0.115753 | legacy_per_cluster | official_1_plus_n, 4 bits |
| MSMARCO | 0.052407 | 0.093949 | legacy_per_cluster | official_1_plus_n, 4 bits |

但这些运行命中了仅保存单个 float 的 epsilon cache，结果中：

```text
safeout_epsilon_cache_hit = true
safeout_epsilon_valid_error_count = 0
safeout_epsilon_attempted_pairs = 0
```

这不表示 epsilon 没有样本，而表示**结果产物没有保留生成该 cache 时的样本量和 calibration manifest**。投稿证据链因此无法从 formal result 自身复核。

## 2.4 query-time 候选区间怎样生成

`src/query/overlap_scheduler.cpp:3557-3564` 先计算：

\[
f_q = 2r_q\hat\epsilon_p.
\]

`src/index/cluster_prober.cpp:217-222` 再使用每个 candidate 实际存储的 `block_norms[j]=r_i`：

\[
e_i=f_q r_i=2r_qr_i\hat\epsilon_p.
\]

这说明：

- 热路径使用的是**候选实际 residual norm**，不是 `r_max`；
- `ConANN` 注释及部分旧设计文档仍写 cluster-level `r_max`，已经过时；
- 采用实际 `r_i` 比 `r_max` 更紧，是合理的工程改进，但论文与代码注释必须统一。

Stage 1 survivor 输出：

```text
est_error = e_i
estimate_lower_bound = d_hat_i - e_i
```

## 2.5 Stage 2 的当前语义

`ClusterProber` 构造时：

\[
\text{margin\_s2\_divisor}=2^{B-1}.
\]

正式 `total_bits=4` 时，Stage 2 候选半径为：

\[
e_i^{(2)}=e_i^{(1)}/8.
\]

随后执行：

\[
\hat d_i^{(2)}-e_i^{(2)}>T_t\Rightarrow \text{SafeOut}.
\]

正式 `legacy_per_cluster` P99 的确是 **Stage 1 base error**，因此当前路径不存在 direct-Stage-2 calibration 后再次 `/8` 的 double scaling。运行时 `/8` 表达的是一条额外假设：多 bit Stage 2 的归一化误差半径相对 Stage 1 至少按 `2^(B-1)` 收缩。

这条规律在旧设计中有明确意图，也可能来自 multi-bit RaBitQ 的误差尺度；但 formal P99 calibration 只测 Stage 1，并没有直接给出 held-out Stage 2 coverage。正确的审稿结论是：

- **已确认**：formal P99 与 Stage 1 FastScan estimator 对齐；
- **未确认**：`epsilon_s2 = epsilon_s1 / 8` 在当前 official 1+n kernel、query quantization 与数据分布下是否达到目标 lower/upper-bound coverage；
- **需要验证但不能先判错**：用 fresh held-out pairs/queries 分别统计 Stage 1 与 Stage 2 violation，并与 direct Stage 2 empirical quantile 对照。

因此本报告把 `/8` 列为 scaling-law coverage audit，而不再称为校准语义冲突或已确认 double scaling。

## 2.6 upper-bound frontier 与 I/O admission

RecordGate 为每个已见、未被 SafeOut 丢弃的候选构造：

\[
U_i=\hat d_i+e_i.
\]

`src/query/overlap_scheduler.cpp:823-874` 维护 `U` 最小的 `k` 个 entry，并令：

\[
T_t=\operatorname{kth\_smallest}_{i\in S_t} U_i.
\]

当 heap 未满时 `T_t=+∞`。对新候选 `x`：

\[
L_x=\hat d_x-e_x>T_t
\]

则跳过 raw-vector I/O；否则将它加入 mandatory verification pipeline。

frontier 在 cluster 入口快照，一整个 cluster 使用同一个 `T_t`；本 cluster 产生的 estimates 在随后才合并，对后续 cluster 生效。

### 条件正确性

若对当前候选 `x` 及构造 frontier 的 witness candidates，所有区间都有效：

\[
L_x\le d_x,\qquad d_j\le U_j,
\]

则 `L_x>T_t` 意味着已经存在至少 `k` 个候选满足：

\[
d_j\le U_j\le T_t<L_x\le d_x.
\]

所以 `x` 不可能进入已见集合的 top-k；未来候选只会进一步把它挤出，不会使它重新进入 top-k。因此在**联合区间成立事件**上，SafeOut 判定是单调且安全的。

这是 RecordGate 最值得写清楚的系统桥接公式。

### 边界

- 保证只针对 IVF 已探测、ANN 已生成的候选集，不保证全库 exact top-k。
- correctness 依赖 candidate lower bound 与至少 `k` 个 witness upper bounds 同时有效。
- frontier 使用 sketch upper bound，不是 exact rerank frontier。
- cluster snapshot 牺牲一部分及时性，但不破坏条件正确性。

---

## 3. 与 RaBitQ 原论文的重合和增量

## 3.1 RaBitQ 已经做过什么

RaBitQ SIGMOD 2024 已经明确提出：

- 一个无偏 inner-product/distance estimator；
- sharp probabilistic error bound；
- 用 lower bound 与当前搜索到的 exact nearest-neighbor distance 比较；
- lower bound 超过 exact frontier 时 drop，否则读取/计算 exact vector 进行 rerank；
- 该策略以高概率把 probed clusters 中的真实 NN 送入 rerank。

原文还明确写道，基于 bound 的 reranking 思想本身不是新的。因此下列陈述都不应成为 RecordGate contribution：

- “首次把 RaBitQ error bound 用于 reranking”；
- “首次由 inner-product bound 得到 distance bound”；
- “首次用 lower bound 排除候选”；
- “首次避免固定 rerank number”。

## 3.2 RecordGate 真正不同之处

与原始 in-memory RaBitQ 的自然对比是：

| 维度 | RaBitQ 原论文 | RecordGate 当前实现 |
|---|---|---|
| 主要资源 | in-memory exact rerank compute | SSD/raw-vector I/O + compute |
| estimator contract | 理论 estimator 与概率 bound | production SIMD estimator + empirical residual calibration |
| frontier | 当前 exact nearest/top-k distance | 已见 estimates 的 kth-smallest upper bound |
| 触发动作 | 是否 exact rerank | 是否提交 raw-vector I/O |
| 执行 | 逻辑 rerank | IVF probing 中异步发起 mandatory reads，并与 span/layout 结合 |
| top-k | 原文以 NN 描述，可推广 | 显式 kth upper-bound heap |

其中最有价值的 delta 是：**在还没有读取 raw vectors 的情况下，用 sketch upper-bound witnesses 自举一个 conservative frontier，把区间支配变成 I/O admission。**

但需要坦率承认：

- top-k 区间支配是简单 order-statistics 推论，不是深的新理论；
- 原始 RaBitQ 已经把 bound 与 rerank 连接起来，概念距离很近；
- 因而 novelty 主要来自它在 storage pipeline 中的端到端设计与效果，而不是公式本身。

## 3.3 采样校准是增强还是削弱理论故事

它是**工程鲁棒性增强**，但在当前形式下是**理论保证削弱**。

增强：

- 校准的是实际 FastScan/query-quantization/official kernel 的总误差，覆盖理论模型与工程实现之间的偏差；
- 可以适应不同 bit-width、SIMD quantization 和数据尺度；
- candidate-specific norm scaling 使区间比 cluster-wide maximum margin 更紧。

削弱：

- 原 RaBitQ theorem 对随机 rotation 给出显式 high-probability bound；当前 empirical P99 没有 finite-sample guarantee；
- calibration query 是 database member pseudo-query，未必代表真实 query distribution，尤其跨模态数据；
- pair 经过 `[0.1d_k,10d_k]` 过滤，范围外不在经验合同中；
- 只有一个全局 epsilon，无法声称 per-vector calibrated confidence；
- 一次 query 中的多候选联合失败概率未控制；
- 使用同一 index/rotation 的 pair 相关性没有建模。

因此应写成“calibration informed by the RaBitQ error decomposition”，而不是“retain RaBitQ's original theoretical guarantee”。

---

## 4. Round 1：严苛审稿意见

### 4.1 总体判断

SafeIn 移除不会使论文失去逻辑闭环。相反，one-sided elimination 更干净：只要候选被排除，它以后不会因 frontier 收紧而重新变成有希望的候选。

但目前试图把“误差采样 → 候选界”抬升为独立创新会遭遇三个直接拒稿点：

1. **prior art overlap**：RaBitQ 原论文已做 error-bound reranking；
2. **guarantee laundering**：经验 P99 被叙述为 RaBitQ theoretical bound；
3. **insufficient probability contract**：pair-level percentile 被暗示为 query-level conservative guarantee。

### 4.2 主要缺口

#### 缺口 A：没有 query-level joint coverage

假设每个 pair 独立且 P99 真有 99% coverage；一条 query 检查 `m` 个候选时，union bound 只能给：

\[
P(\text{all intervals valid})\ge 1-m(1-0.99).
\]

当 `m>100` 时该下界已经无意义。现实候选还相关，不能用独立性乐观修复。

SafeOut 需要两类事件同时成立：

- 被排除候选的 lower bound 不超过真实距离；
- frontier witnesses 的 upper bounds 不低于真实距离。

所以简单报告 marginal coverage 或最终 recall 不足以支撑“certificate”。

#### 缺口 B：FastScan Stage 1 与原始理论 estimator 不同

`src/rabitq/rabitq_estimator.cpp:410-436` 的 1-bit path 直接使用 sign vector inner product；原始 RaBitQ estimator 包含对 data vector 与 quantized vector alignment 的校正，以获得无偏性。Stage 1 FastScan 还量化 query。当前实现依靠经验 calibration 吸收这些误差，因此不能原样引用 RaBitQ theorem 作为 Stage 1 guarantee。

#### 缺口 C：Stage 2 的 `/8` 缩放尚缺直接 coverage 证据

formal `legacy_per_cluster` epsilon 已与 Stage 1 FastScan 对齐。剩余问题是 runtime 把 Stage 1 radius 除以 8 后，official Stage 2 的 held-out lower/upper-bound violation 是否仍满足目标置信口径。该项需要验证，但现有代码审查不能将其判定为 double scaling bug。

#### 缺口 D：缓存缺少 provenance

formal runs 只记录 cache hit 和 float 值，没有：

- calibration dataset/query source；
- valid pair count；
- random seed；
- estimator/stage；
- bit width；
- distance filter；
- empirical violation curve；
- code commit/version。

这使结果不可审计。

#### 缺口 E：现有强消融不回答创新问题

NoSafeOut 大幅增加 latency/read work，只说明 pruning 比 verify-all 必要，不能证明：

- candidate-specific interval 优于 global fixed margin；
- kth-upper frontier 优于 RaBitQ-style exact frontier；
- empirical calibration 优于 theoretical/default constant；
- dynamic bound policy 优于 matched-recall fixed-R。

### 4.3 Round 1 分数

- 技术正确性：`4/10`，因为核心确定性推论成立，但概率前提未闭合。
- 新颖性：`4/10`，与 RaBitQ 原始 reranking 高度重合。
- 系统意义：`7/10`，raw-vector I/O reduction 确实是重要问题。
- 证据充分性：`4/10`，NoSafeOut 很强，但缺少关键基线与 coverage audit。
- Round 1 verdict：**Weak Reject / Major Revision**。

---

## 5. 对 Round 1 的回应与重构提案

### 5.1 不再争夺 RaBitQ 理论创新

将贡献从：

> We derive per-vector RaBitQ error bounds and use them for pruning.

改为：

> RecordGate turns calibrated distance intervals from the deployed RaBitQ kernels into storage admission decisions. It maintains an online top-k upper-bound frontier over candidates observed during IVF probing and submits raw-vector reads only for candidates whose lower bounds are not dominated.

这个改写把 prior-art ownership 留给 RaBitQ，同时明确 RecordGate 的系统 delta。

### 5.2 将 epsilon 重新定义为 calibration contract

建议论文定义：

\[
z_i^+(q)=\frac{\hat d_i-d_i}{2r_ir_q},\qquad
z_i^-(q)=\frac{d_i-\hat d_i}{2r_ir_q}.
\]

SafeOut lower bound 需要控制 `z^+`，frontier upper witnesses 需要控制 `z^-`。可以：

- 最简单：仍用 absolute error `max(z^+,z^-)`，但做 query-level maximum calibration；
- 更紧：分别校准 `epsilon_L` 与 `epsilon_U`，形成非对称区间：

\[
L_i=\hat d_i-2r_ir_q\epsilon_L,
\]

\[
U_i=\hat d_i+2r_ir_q\epsilon_U.
\]

对每个 calibration query，收集该 query 的关键最大 violation：

\[
Z_q=\max_{i\in C(q)}\max(z_i^+,z_i^-),
\]

再在 query 维度取 finite-sample corrected quantile。这样才能合理地写：

> Under exchangeability between calibration and serving queries, the candidate intervals enjoy marginal query-level coverage over the probed candidate set.

这里的 `marginal query-level` 仍不是对 distribution shift 的无条件保证，但比 pair P99 严谨得多。

### 5.3 把 deterministic lemma 与 probabilistic calibration 分开

论文应把核心合同拆成两层：

1. **Deterministic dominance lemma**：如果所有参与判定的 intervals 有效，则 `L_i>T_t` 的候选不可能进入已见/未来候选 top-k。
2. **Calibration coverage statement**：描述 intervals 在何种 query distribution、candidate scope 和置信参数下有效。

不要把两者合成“SafeOut is safe”。

### 5.4 与物理执行闭环

这条机制最好与另一条物理贡献共同叙述：

```text
RaBitQ-derived estimates
  -> calibrated intervals
  -> online top-k interval dominance
  -> mandatory raw-vector read set
  -> amplification-bounded span planner
  -> payload-byte reuse
```

这样，RaBitQ 部分负责“读哪些候选”，span/layout 部分负责“怎样读这些候选”。这比把 SafeOut 与 span 当成互不相关的两个算法更有系统论文完整性。

---

## 6. Round 2：重构后的再评审

### 6.1 重构是否缓解 prior-art 问题

是，但只能部分缓解。

- 承认 RaBitQ 已有 bound-based reranking 后，novelty 争议显著降低。
- kth upper-bound frontier 在未读取 raw vectors 前自举 verification threshold，是一个合理且清楚的 delta。
- 将 decision 接到 asynchronous raw-vector I/O，而非只决定 CPU rerank，具有系统意义。
- 但 top-k order-statistic 本身很直接，不能夸大为重大理论突破。

### 6.2 query-level calibration 是否值得在两周内实现

值得，而且优先级高于继续优化 SafeIn 或发明更复杂的 bound。

实现成本预计低到中：现有 calibrator 已经遍历 query-target pair，只需把误差先按 calibration query 聚合为 maximum/critical-maximum，再取 corrected quantile，并输出 manifest。无需改 query hot path。

它可以同时解决：

- probability contract；
- cache provenance；
- P95/P99 参数解释；
- reviewer 对“conservative”的质疑。

但必须注意：如果只有约 100 个独立 calibration queries，就无法稳健估计 `δ=10^-3` 的 query-level quantile。对 `δ=0.01` 也只能提供粗粒度证据。应使用至少 1,000 个真实、与测试隔离的 calibration queries，或降低承诺强度。

### 6.3 重构后的可接受贡献边界

#### 可以作为 contribution bullet 的版本

> **Bound-to-I/O verification control.** RecordGate calibrates the errors of its deployed RaBitQ kernels and maps each candidate estimate to a norm-scaled distance interval. During IVF probing, it maintains the kth-smallest candidate upper bound as an online verification frontier and suppresses raw-vector reads whose lower bounds are dominated. Conditioned on interval validity, the exclusion decision is monotone and cannot remove a candidate that may enter the top-k of the probed candidate set.

前提：完成 query-level calibration audit，并将 `conditioned on interval validity` 与 candidate-set scope 明写。

#### 当前代码不修改时只能写的版本

> RecordGate uses empirically calibrated, candidate-dependent distance margins to reduce raw-vector verification I/O. We evaluate the resulting recall–I/O trade-off against fixed reranking budgets.

不能出现 `guarantee`、`certificate`、`theoretically safe`、`query-level 99%`。

#### 不允许写

- “We propose a new RaBitQ error bound.”
- “We are the first to use RaBitQ bounds for reranking.”
- “P99 ensures a 1% query failure rate.”
- “SafeOut has no false negatives.”
- “The method preserves exact global top-k.”
- “Each vector receives an individually estimated confidence bound.”

### 6.4 Round 2 分数

假设完成 P0 correctness package：

- 技术正确性：`7/10`。
- 新颖性：`6/10`，作为系统机制而非新量化理论。
- 系统意义：`8/10`。
- 证据充分性：`7/10`。
- Round 2 verdict：**Weak Accept as part of a complete system paper**。

若不补 probability/stage audit：仍为 **Weak Reject**。

---

## 7. 最小实验与正确性包

## P0-A：fresh calibration + stage consistency audit

数据集：Amazon ESCI、MSMARCO；若时间允许加 COCO。  
配置：与 formal main experiment 完全相同的 `official_1_plus_n,total_bits=4`、nprobe、topk、query split。  

必须绕过旧 scalar cache，保存完整 manifest：

- calibration query IDs 与数量；
- valid pair count / attempted pair count；
- estimator stage；
- bit width、rotation/index ID、code version；
- seed、distance filter；
- epsilon quantile 与有限样本 rank；
- calibration wall time。

在 held-out queries 上逐 candidate 报告：

- `Pr(d_i < L_i)`：lower-bound violation；
- `Pr(d_i > U_i)`：upper-bound violation；
- `Pr(any violation in query)`：query-level violation；
- true top-k 被 SafeOut 的数量；
- 每 query harmful false-prune rate；
- Stage 1 与 Stage 2 分开统计。

必须比较：

1. 当前 cached P99；
2. fresh pair-P99；
3. query-max calibrated epsilon；
4. Stage 2 direct epsilon vs `Stage1 epsilon / 8`。

胜出条件：选择能满足目标 recall/coverage、同时验证读取量最低的合同；若 `/8` 不能在 held-out 上达到目标 coverage，应删除该缩放或做 stage-specific epsilon。

## P0-B：matched-recall fixed-R frontier

对 Amazon ESCI、MSMARCO 运行：

- fixed rerank depth `R ∈ {k,2k,4k,8k,...}`，覆盖当前 dynamic rerank count；
- RecordGate interval frontier；
- NoSafeOut/verify-all 只作为上界。

报告：

- recall@10/recall@k；
- raw-vector candidates、requests、bytes；
- avg/p95/p99 latency、QPS；
- calibration/storage overhead。

只有在 matched recall 下读更少或延迟更低，才能声称 dynamic evidence 优于 static rerank budget。

## P0-C：frontier 与 margin 的机制消融

至少包含：

1. **Fixed-R**：传统静态预算。
2. **Global fixed distance margin**：不乘 candidate `r_i`，用于证明 candidate scaling。
3. **Candidate interval + exact frontier**：RaBitQ-style，在已有 exact reads 后更新 frontier。
4. **Candidate interval + kth upper-bound frontier**：RecordGate。
5. **NoSafeOut**：verify-all。

核心结果不是仅看 QPS，而是：在同 recall/coverage 下，RecordGate frontier 能否更早稳定、少提交多少 raw-vector I/O。

## P1：理论常数 vs empirical calibration

若能从当前 official RaBitQ implementation 确定理论 `epsilon_0` 到 distance radius 的准确映射，则比较：

- RaBitQ theoretical constant；
- build-time P95；
- runtime pair-P99；
- query-level calibration。

如果 estimator 不完全匹配原 theorem，则把理论常数只作为参考，不声称严格 baseline。

## 预计成本

- P0-A：主要是 CPU calibration + vector-only replay，约 0.5–1.5 天工程，数小时运行。
- P0-B：可复用索引与 query assets，约 0.5 天脚本，1–2 天运行/汇总。
- P0-C：如果 exact frontier baseline 已有代码路径，约 1 天；否则可先做 trace replay，避免侵入 production hot path。
- 总体可在两周内完成，优先级显著高于 SafeIn 新 sweep。

---

## 8. Claims matrix

| 证据状态 | 允许的主张 | 禁止的主张 |
|---|---|---|
| 只有现有 P99 + NoSafeOut | 经验校准 margins 显著减少 verify-all raw-vector work | 理论安全、query-level 99%、优于 fixed-R |
| fresh pair coverage 通过，但 query-level coverage 未通过 | candidate intervals have high marginal pair coverage on held-out pairs | query-level certificate、无 false pruning |
| query-max calibration 达目标 coverage | conditioned on the calibrated query-level event, interval dominance safely suppresses reads in the probed candidate set | global exact top-k、distribution-shift robustness |
| fixed-R matched recall 胜出 | dynamic interval frontier reduces raw-vector I/O at matched recall relative to static rerank depths | 所有 workload 普遍胜出，除非四数据集均稳定成立 |
| candidate scaling ablation 胜出 | norm-scaled intervals are tighter/more efficient than a global worst-case margin | per-vector learned uncertainty |
| kth-upper frontier 胜 exact-frontier/estimated-distance frontier | upper-bound witnesses tighten verification admission before exact reads | 新的通用 top-k 算法或复杂理论突破 |
| `/8` coverage 失败 | 使用 stage-specific empirical epsilon，删掉旧 scaling claim | 保留错误的 bit-scaling 理论叙述 |
| query-level calibration 过于保守导致收益消失 | 将机制降级为 empirical recall–I/O knob | conservative guarantee |

---

## 9. 推荐论文结构与措辞

### 9.1 方法小节结构

1. **Problem: verification I/O admission**  
   quantized search 产生大量候选，而 raw vector 在慢层；固定 R 不能利用候选不确定性。

2. **Deployed estimator calibration**  
   明确校准实际 kernel 的 normalized residual，说明 query split 与 probability scope。

3. **Candidate-dependent intervals**  
   推导 `e_i=2r_ir_q epsilon`，强调 epsilon global、radius candidate-dependent。

4. **Online top-k upper-bound frontier**  
   定义 `T_t=kth_smallest(U)` 和 `L_i>T_t`。

5. **Conditional correctness lemma**  
   区分 deterministic dominance 与 calibration coverage。

6. **I/O execution**  
   surviving candidates 进入 mandatory vector-read pipeline，再由 span planner 合并。

### 9.2 推荐 contribution bullet

> We design a bound-to-I/O verification controller that calibrates the deployed RaBitQ kernels, constructs candidate-dependent distance intervals, and maintains an online top-k upper-bound frontier during IVF probing. Candidates whose lower bounds are dominated do not trigger raw-vector reads; conditioned on interval validity, this exclusion is monotone within the probed candidate set.

### 9.3 推荐中文叙述

> RecordGate 并不重新发明 RaBitQ 的误差界，而是解决误差界如何转化为存储访问决策的问题。系统对实际部署的 RaBitQ 估计 kernel 进行离线校准，并结合查询与候选的残差范数构造候选相关距离区间。在 IVF 探测过程中，RecordGate 维护已见候选第 k 小的距离上界作为在线验证 frontier；仅当候选下界仍可能进入该 frontier 时，系统才提交原始向量读取。该设计将量化不确定性与 raw-vector I/O admission 连接起来，并为后续读取合并提供必要读取集合。

### 9.4 术语替换

| 不推荐 | 推荐 |
|---|---|
| RaBitQ theoretical bound | calibrated RaBitQ-derived interval |
| per-vector error estimate | candidate-dependent norm-scaled radius |
| safe pruning / SafeOut certificate | confidence-scoped exclusion / interval-dominated candidate |
| guarantees no false reads/prunes | conditioned on interval validity |
| query-level P99 | empirical pair-P99，直到 query-level calibration 完成 |
| exact top-k guarantee | probed-candidate-set verification guarantee |

---

## 10. 优先 TODO

### P0，投稿前必须完成

1. 对 official 1+n formal 配置做 fresh Stage 1/Stage 2 violation audit，验证 `legacy_per_cluster` Stage 1 P99 以及 Stage 2 `/8` 缩放各自的 held-out coverage；不要再把 formal 路径描述成 direct-S2 calibration。
2. 为 epsilon cache 加 manifest；formal result 必须记录真实 valid sample count，不允许 cache hit 时归零。
3. 用 held-out real queries 做 query-level coverage；若不实现 query-max calibration，就删除所有 query-level guarantee 文案。
4. 跑 matched-recall Fixed-R baseline。
5. 统一代码注释、方法章节：runtime 使用 candidate `r_i`，不是 cluster `r_max`。

### P1，显著增强论文

6. `global worst-case margin` vs `candidate norm-scaled margin` 消融。
7. `exact frontier` vs `kth upper-bound frontier` 消融或 trace replay。
8. 分别校准 lower/upper one-sided errors，减少 absolute symmetric interval 的保守性。

### P2，可延期

9. distribution shift / cross-domain calibration。
10. online recalibration 或 drift detection。
11. cluster-conditioned/query-feature-conditioned epsilon；两周期限下不建议开展。

---

## 11. Mock ICDE/PVLDB review

### Summary

The paper presents RecordGate, a vector retrieval system that couples quantized candidate generation with SSD-resident raw-vector verification. After removing an ineffective speculative SafeIn path, the proposed controller calibrates the errors of deployed RaBitQ-derived estimators, converts candidate scores into norm-scaled distance intervals, maintains the kth smallest upper bound observed during IVF probing, and suppresses raw-vector reads for candidates whose lower bounds are dominated. The surviving reads are executed by a separate coalescing and layout-aware pipeline.

### Strengths

1. The paper targets a real systems bottleneck: exact-vector verification turns quantization uncertainty into many SSD reads.
2. The interval dominance rule is simple, implementable, and cleanly connected to raw-vector I/O admission.
3. Maintaining an upper-bound frontier before exact reads is a useful system adaptation beyond the original in-memory RaBitQ reranking description.
4. The approach composes naturally with the paper's span planning and record layout mechanisms.
5. Existing NoSafeOut measurements suggest that verification-work reduction is highly consequential.

### Weaknesses

1. RaBitQ already provides probabilistic distance bounds and explicitly uses them to decide reranking. The paper must not claim this general idea as novel.
2. The deployed Stage 1 estimator and empirical P99 calibration do not obviously satisfy the original RaBitQ theorem. The current text risks conflating a theoretical bound with an empirical quantile.
3. Pair-level P99 calibration does not imply that all intervals used by one query are simultaneously valid. The claimed query-level safety contract is therefore unsupported.
4. The formal `legacy_per_cluster` calibration measures Stage 1 FastScan errors. The runtime then derives the official Stage 2 radius by dividing the Stage 1 radius by eight, but the paper currently lacks direct held-out evidence that this scaling achieves the stated Stage 2 coverage.
5. Formal runs use cached epsilon values whose provenance and realized sample counts are not retained in the result artifacts.
6. NoSafeOut is not an adequate baseline for the claimed advantage over fixed reranking depths.

### Questions for authors

1. What is the exact probability space of the claimed bound: random rotation, sampled candidate pairs, or serving queries?
2. Does epsilon provide pair-level or query-level coverage, and how are multiple candidate comparisons handled?
3. What theoretical or empirical evidence supports deriving the official Stage 2 margin as the Stage 1 FastScan margin divided by `2^(B-1)`?
4. How many independent calibration queries produced each cached epsilon, and are these queries disjoint from evaluation?
5. How much does candidate norm scaling improve over a global or cluster-level margin?
6. At matched recall, does the upper-bound frontier read fewer raw vectors than fixed-R and the original RaBitQ-style exact frontier?

### Score

- Current manuscript: **4/10, Weak Reject**.
- After the P0 package and restrained positioning: **6/10, Weak Accept**.
- Confidence: **4/5**.

### What would move the paper toward accept

1. Separate the deterministic dominance lemma from the statistical coverage statement.
2. Establish query-level held-out coverage or remove safety language.
3. Resolve Stage 1/Stage 2 calibration consistency.
4. Add a matched-recall fixed-R baseline and a frontier ablation.
5. Frame the contribution as storage-facing bound-to-I/O control, not as a new RaBitQ error bound.

---

## 12. 最终 verdict

### 对用户核心问题的直接回答

**可以写入，但不能按“我们提出了从 RaBitQ 误差到单向量距离界”来写。** 这部分大多属于 RaBitQ 已有理论与直接误差传播。RecordGate 可写的创新增量是：

> 对真实部署 estimator 进行校准，并将候选相关区间与一个无需预先读取 raw vector 的 top-k upper-bound frontier 结合，直接控制存储 I/O admission。

它是 RaBitQ error bound 的一个有系统价值的**新应用/系统化改造**，不是新的 RaBitQ bound。若仅保留现有 pair-P99，则称“empirical application”；若补 query-level calibration、条件正确性定理和 matched-recall 证据，则可升级为论文的一条完整系统贡献。

### SafeIn 移除后的创新强度

- SafeIn 的移除不会使这条线失去价值；one-sided elimination 反而更易形成清楚合同。
- 但 RaBitQ pruning 不能独立撑起整篇论文，必须与 span planner、record layout 和 overlapped execution 形成端到端闭环。
- 最强叙述不是“三个平行小点”，而是：

```text
which records to read  ->  how to group the reads  ->  what payload bytes to reuse
interval admission        bounded span planning       co-located record layout
```

在该闭环中，本机制负责第一步，创新强度足够；把它包装成新的 RaBitQ 理论则会适得其反。
