# RaBitQ Stage2 新格式与论文贡献定位评审

日期：2026-06-15  
外部评审模型：GPT-5.5，reasoning effort = xhigh  
评审轮次：2  
评审线程：`019ec6df-af55-7c20-b81f-f944f7fb3880`

## 1. 最终结论

当前的新 Stage2 数据格式与直接紧凑码计算方案：

- **不能作为独立的顶级数据库会议贡献。**
- **可以作为第二贡献“决策对齐的渐进式物理格式”的候选子机制。**
- 在尚未直接对比 RaBitQ Library 官方 packing 和 kernel 前，只能称为
  **implementation hypothesis**，不能称为已经成立的格式创新。
- 即使后续全面优于官方实现，也更适合作为第二贡献的核心机制，而不是第四个独立贡献。

当前判断及置信度：

| 实验证据状态 | 建议定位 | 置信度 |
|---|---|---:|
| 当前：只优于部分自研旧布局，尚无官方直接基线 | 第二贡献的次要候选机制 | 95% |
| 补齐官方基线，但只在 ESCI 上胜出 | 第二贡献的次要机制 | 90% |
| 在可预测的低 survivor-density 区间稳定优于官方基线 | 第二贡献的核心机制 | 80% |
| 跨数据集全面胜出 | 仍不足以成为独立贡献 | 90% |

最稳妥的论文组织不是三个完全平行的“小贡献”，而是：

1. 一个核心抽象：**界驱动的渐进式记录物化决策**。
2. 两类协同机制：**决策对齐的物理组织**与**界感知的异步执行**。

## 2. 当前方法与官方 RaBitQ 的边界

### 2.1 官方已经具备的能力

RaBitQ Library 已经支持：

- 将量化码拆成 binary code/MSB 和 extended code。
- 先用 binary code 估计，在必要时访问 ex-code 做增量细化。
- 对 1--8 bit ex-code 使用专用紧凑格式。
- 维度按 64 对齐，并为 2-bit、3-bit、4-bit 等提供专用 SIMD 内积实现。

因此，以下内容不能作为本文创新：

- `1+n` RaBitQ 语义。
- Stage1/Stage2 拆分。
- ex-code bit packing。
- 按需访问 ex-code。
- 使用 SIMD 计算多 bit ex-code。
- RaBitQ 的距离估计器和误差界。

### 2.2 本文新实现的真实差异

当前 `vector_bitmajor_tiles` 的差异集中在物理访问与执行协同：

1. 每个候选向量的 Stage2 码连续存储。
2. 向量内部按维度 tile 划分，每个 tile 内按 bitplane 排列。
3. tile 维度根据剩余维度选择 512/256/128/64，使单 bitplane 接近
   64/32/16/8 B。
4. 查询只对误差界判定为 uncertain 的 candidate lane 执行 Stage2。
5. AVX-512 内核直接从紧凑 bitplane 做 mask-accumulate，不物化
   `uint8_t[dim]` 中间数组。
6. Stage2 细化后，候选可沿同一 cluster-local ordinal 继续访问
   `data.dat` 中的原始向量和 payload。

准确表述应是：

> 避免物化逐维展开的 Stage2 中间表示，并使稀疏候选细化的物理访问粒度
> 与后续原始记录访问保持一致。

不应表述为“消除解码”，因为查询内核仍然需要解释 bitplane，只是将
bit extraction 与点积融合。

## 3. 实验证据审计

### 3.1 当前结果

| 数据集 | total bits | QPS 变化 | Stage2 时间变化 | 结论 |
|---|---:|---:|---:|---|
| Amazon ESCI | 2 | +6.56% | -1.93% | 端到端收益无法由 Stage2 计时解释 |
| Amazon ESCI | 3 | +5.64% | -10.24% | 正向，但需要关键路径诊断 |
| Amazon ESCI | 4 | +6.84% | -15.67% | 当前最强正向证据 |
| MSMARCO | 2 | +1.42% | -28.82% | Stage2 收益被其他阶段抵消 |
| MSMARCO | 3 | +0.74% | -24.45% | 端到端收益很小 |
| MSMARCO | 4 | +0.75% | -23.44% | 端到端收益很小 |
| COCO100k | 2 | -0.36% | 小幅改善 | 基本持平 |
| COCO100k | 3 | +0.17% | 小幅改善 | 基本持平 |
| COCO100k | 4 | -2.65% | 退化 | 负优化 |

索引码空间和 peak RSS 改善通常低于 1%，因此不能主张压缩或显著内存收益。

### 3.2 最大的因果漏洞

ESCI 的总延迟节省与 Stage2 计时器记录的节省不闭合：

| bits | 总延迟节省 | Stage2 节省 | 未被 Stage2 解释 |
|---:|---:|---:|---:|
| 2 | 0.068651 ms | 0.002133 ms | 0.066518 ms |
| 3 | 0.054548 ms | 0.009760 ms | 0.044788 ms |
| 4 | 0.064435 ms | 0.015320 ms | 0.049115 ms |

MSMARCO 则呈相反情况：Stage2 节省约 0.044--0.051 ms，但端到端只节省
0.018--0.042 ms。

当前只能写“观察到相关的端到端变化”，不能写“新 Stage2 格式导致了
5.6%--6.8% 的端到端提升”。必须检查：

- 计时器是否覆盖全部 decode、地址计算和 memory stall。
- 新旧布局是否改变 cache state，进而影响后续阶段。
- 是否存在 instrumentation、编译、绑核、CPU 频率或运行顺序差异。
- overlap 模式下，Stage2 是否位于关键路径。
- final drain、queue wait、I/O wait 和后续 rerank 是否发生变化。

## 4. 推荐的统一抽象

建议将第二贡献抽象为：

## 决策对齐的渐进式记录物化

核心原则：

> 随着候选集合逐步缩小，系统只允许每个候选逐步访问更多数据；每个决策
> 状态对应一个最小物理片段和一个明确的调度动作。

### 4.1 设计不变量

1. 候选只沿物化状态单调推进，不回退、不重复读取已物化内容。
2. 同一 cluster-local ordinal 在 Stage1、Stage2、原始向量和 payload
   之间保持身份一致。
3. 每次状态转换只访问下一次决策所需的最小数据片段。
4. 物理访问粒度与候选密度匹配：
   - 密集候选使用批量 Stage1 布局。
   - 稀疏 uncertain 候选使用 vector-local Stage2 布局。
   - 最终少量候选使用逐记录原始向量和 payload 访问。
5. 格式变化不能改变 RaBitQ 分数、误差界和候选语义。

### 4.2 数据访问状态机

| 状态 | 已物化数据 | 可能转换 |
|---|---|---|
| `I0` | Stage1/MSB | SafeOut -> `DROP`；uncertain -> `I1`；高置信推测读取 -> `R` |
| `I1` | Stage2 ex-code | SafeOut -> `DROP`；需要精确验证 -> `V`；高置信 -> `R` |
| `V` | 原始向量 | 精确淘汰 -> `DROP`；进入最终结果 -> `R` |
| `R` | payload/完整记录 | 返回结果 |
| `DROP` | 无 | 终止，不再访问 |

新 Stage2 tile 格式的价值不在于单独“更紧凑”，而在于它是 `I0 -> I1`
稀疏状态转换的物理实现；地址直达记录则负责 `I1 -> V -> R`。

### 4.3 仍然只是实现细节的内容

- 512/256/128/64 的具体 tile 数值。
- `_mm512_maskz_mov_ps` 和 16 维循环。
- bitplane 在 tile 内的具体次序。
- 文件 magic、对齐字节数、队列数量和 I/O API。
- 仅仅使用 base offset 或 ordinal arithmetic。

只有当这些选择由访问密度模型解释，并且相对合理基线有稳定收益时，
才能从实现细节升级为可讨论的设计机制。

## 5. 推荐贡献结构

### 5.1 最稳妥的两项结构

1. **界驱动的渐进式记录物化策略。**  
   基于 RaBitQ 已有误差界和动态 top-k frontier，决定候选终止于索引、
   继续 Stage2、读取原始向量，还是推测读取 payload。

2. **决策对齐的存储与执行协同设计。**  
   在共享 cluster-local ordinal 下组织 Stage1、Stage2、原始向量和
   payload，并用异步流水线执行状态转换。Stage2 tile 是该机制的一种
   物理实现。

这版最不容易被认为是 salami slicing。

### 5.2 有条件可用的三项结构

1. **界驱动候选访问策略。**
2. **支持渐进物化的 ordinal-addressed record organization。**
3. **面向物化状态转换的异步调度器。**

采用三项结构的前提是贡献 2 和贡献 3 都有独立、合理且强的基线与消融。
不能再把 Stage2 tile 或 SIMD kernel 单独拆成贡献。

### 5.3 可直接用于 Introduction 的表述

1. **界驱动的渐进式物化。**  
   在 RaBitQ 已有增量估计器和误差界的基础上，我们将 record-return ANN
   建模为一系列候选物化决策，动态决定候选应被淘汰、使用 extended code
   细化、读取原始向量验证，或推测读取 payload。

2. **决策对齐的存储与执行。**  
   我们在共享的 cluster-local ordinal 下组织索引片段、原始向量和
   payload，并通过异步流水线执行它们之间的状态转换。该组织分别匹配
   密集 Stage1 扫描、稀疏 Stage2 细化和逐记录返回的访问粒度，同时避免
   独立的全局 ID-to-payload lookup。

在官方基线胜出前，不应在 Introduction 的贡献 bullet 中突出
`vector_bitmajor_tiles`。

## 6. Claim Matrix

| Claim | 当前状态 | 必须补充的证据 | 失败后的降级表述 |
|---|---|---|---|
| 新格式保持 RaBitQ 数学和候选语义 | 基本支持 | 逐查询 score、mask、候选集合一致性 | 仅作为实现正确性说明 |
| 新格式优于自研旧 Stage2 布局 | ESCI 支持 | MSMARCO 公平复核、置信区间、完整 COCO 负结果 | “在 ESCI 上有效” |
| 新格式优于官方 compact packing+kernel | 未证明 | 同执行器、同 code、同 mask 的官方直接基线 | 删除性能创新 claim |
| 端到端收益由新格式导致 | 当前不成立 | 完整时间闭环和硬件计数器 | 只报告相关性 |
| ordinal 直达优于 ID+payload store | 未证明 | dense map、RID、offset array、FlatStor/Lance 对比 | 降为实现选择 |
| 新格式可作为统一默认 | COCO 已反驳 | 跨 workload 稳定胜出 | workload-specific option |
| survivor-aware 自动布局选择 | 尚无实现 | 密度 sweep、成本模型、selector regret | 手工配置选项 |
| 新格式显著压缩索引或 RSS | 证据否定 | 不建议继续投入 | 明确不主张 |

## 7. 投稿前最低实验包

### 7.1 必须完成

1. **官方 RaBitQ 直接基线**
   - 将官方 per-vector compact packing 和最佳 kernel 嵌入同一执行器。
   - 保持量化码、candidate mask、候选顺序、编译参数和线程设置相同。
   - 对比官方格式、自研旧格式和 `vector_bitmajor_tiles`。

2. **布局和 kernel 二维消融**
   - 官方格式 + 官方 kernel。
   - 官方格式 + 本文可兼容 kernel。
   - 新格式 + 展开后计算。
   - 新格式 + direct compact kernel。

3. **因果计时闭环**
   - resident/no-I/O 模式隔离 CPU 开销。
   - overlap 模式记录提交、queue wait、I/O wait、final drain 和 rerank。
   - paired per-query A/B，随机交错运行，绑核并固定 CPU 频率。
   - 至少 10 次重复，报告置信区间以及 p50/p95/p99。

4. **微架构证据**
   - cycles、instructions、IPC。
   - L1/LLC miss。
   - branch miss。
   - frontend/backend stall。
   - memory bandwidth 和实际访问字节数。

5. **survivor-density sweep**
   - 控制进入 Stage2 的候选比例。
   - 同时覆盖不同维度、ex_bits 和候选 batch 密度。
   - 给出新格式胜出和退化的 break-even 区间。

6. **payload 与地址访问基线**
   - payload 取 0、仅向量、1 KB、4 KB、64 KB、1 MB。
   - 对比 cluster-local ordinal、dense ID-offset map、常规 RID/offset-array，
     并保留 FlatStor/Lance 端到端基线。

7. **完整公开 COCO 负结果**
   - 不将新格式设为无条件默认。
   - 若要自动选择布局，需要离线校准 selector，并报告相对 oracle 的 regret。

8. **完整系统消融**
   - No candidate policy。
   - No decision-aligned layout。
   - No pipeline。
   - No Stage2 tile specialization。

粗略成本：官方基线适配与二维消融约 1--2 个工程日；三数据集公平重复、
密度 sweep 和 perf 诊断约 1--3 个 CPU 日，具体取决于是否重建格式索引。

### 7.2 可放附录

- 64/128/256/512/full-vector tile sweep。
- 第二代 AVX-512 CPU 和 AVX2 fallback。
- 失败的 microbatch、small-lane 和 prefetch 布局。
- 逐查询延迟分布、index size 和 RSS 明细。

## 8. 两轮外部评审摘要

### 8.1 第一轮

第一轮认为新格式是有价值的 format-specific kernel co-design，但不足以构成
独立贡献。它的研究价值来自与候选选择和原始记录访问的协同，而不是
bit packing 或 SIMD 本身。

第一轮给该机制的评分：

| 维度 | 分数 |
|---|---:|
| Novelty | 2/5 |
| Technical depth | 3/5 |
| Evaluation | 2/5 |
| Overall | 2/5，Weak Reject |

若把它单列为创新点，第一轮估计拒稿风险超过 80%。

### 8.2 第二轮

第二轮进一步收紧结论：由于尚未直接对比官方 packing+kernel，目前连
“有限但成立的物理格式创新”也不能确认。最危险的审稿意见会是：

> 新设计只是修复了作者旧有 interleaved/split 布局的低效，而官方方案原本
> 已经使用合理的逐向量紧凑 ex-code。

第二轮因此要求：

- 在官方直接基线完成前，只称为 candidate-selective implementation。
- 将论文组织为一个核心抽象加两个协同机制。
- 用 survivor density 和关键路径诊断解释何时有效。
- 若官方基线持平或更快，则从贡献段删除，只保留在实现或附录。

## 9. Go / No-Go

结论是 **有条件 GO**。

只有同时满足以下条件，才建议把新 Stage2 格式保留在论文主线：

1. 在相同执行器中显著优于官方 RaBitQ packing+kernel。
2. 端到端收益能够由关键路径和微架构证据解释。
3. survivor density 能预测 ESCI/MSMARCO 收益与 COCO 退化。
4. 存在明确适用区间或 selector，避免把负优化设为默认。

若官方基线持平或更快，或 ESCI 的未解释收益无法闭环，则应：

- 从贡献列表中删除 Stage2 新格式。
- 将其降级为实现章节中的 workload-specific optimization。
- 第二贡献只保留渐进式记录组织、cluster-local ordinal 和原始记录访问。

## 10. 参考资料与证据位置

官方资料：

- RaBitQ 原论文：https://arxiv.org/abs/2405.12497
- RaBitQ Library：https://github.com/VectorDB-NTU/RaBitQ-Library
- RaBitQ Library paper：https://openreview.net/pdf?id=OeZHhOsFir
- Compact code 文档：https://vectordb-ntu.github.io/RaBitQ-Library/compact_code/
- IVF 文档：https://vectordb-ntu.github.io/RaBitQ-Library/index/ivf/

本地实验摘要：

- `/home/zcq/VDB/test/bitmajor_tiles_20260609/summary_coco_old_vs_bitmajor_notiming_final.csv`
- `/home/zcq/VDB/test/bitmajor_tiles_20260609/summary_esci_aligned_fair_repeats.csv`
- `/home/zcq/VDB/test/bitmajor_tiles_20260609/summary_esci_msmarco_old_split_vs_bitmajor_notiming.csv`

本地实现：

- `src/simd/ip_exrabitq.cpp`
- `src/index/cluster_prober.cpp`
- `src/storage/cluster_store.cpp`
- `include/vdb/common/types.h`
