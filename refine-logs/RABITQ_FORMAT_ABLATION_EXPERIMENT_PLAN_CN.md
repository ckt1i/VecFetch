# RaBitQ 内存/格式优化消融实验计划

日期：2026-06-22

## 结论

这一轮优化可以只跑 1--2 个数据集，前提是论文定位明确：

- 不把新 Stage2 格式写成独立贡献。
- 只把它作为第二贡献“决策对齐的渐进式物理格式”的子机制消融。
- 已有五数据集主结果、六组 baseline 和 Full VecFetch Pareto 继续作为主实验依据。

推荐正文最小组合：

1. `amazon_esci`：结构化 record-return 主数据集，历史结果显示新布局端到端收益最明显。
2. `msmarco_passage`：文本主数据集，历史结果显示 Stage2 自身收益明显但端到端收益小，适合解释 Amdahl/关键路径边界。

如果只能跑一个数据集，先跑 `amazon_esci`。

`coco_100k` 不建议替代 `msmarco_passage` 进入正文，因为它在 ICDE 计划中只作为旧论文回归和 appendix；但它适合作为便宜的 appendix sanity/negative case，用来说明新格式不是无条件默认。

## Claim Map

| Claim | Why It Matters | Minimum Convincing Evidence | Linked Blocks |
|---|---|---|---|
| C1: 新 Stage2 物理布局在不改变 RaBitQ 语义的前提下，降低 Stage2 resident memory 和不确定候选细化成本 | 支撑第二贡献中的渐进式物化格式，而不是新的量化算法 | 与官方 RaBitQ compact packing/kernel、旧布局、新布局在相同 candidate mask 下对比；recall/candidate 集合一致；RSS/code bytes 更低，Stage2 或关键路径不退化 | B1, B2 |
| C2: 新布局的收益取决于 survivor density 和关键路径，而不是普适加速 | 避免被审稿人质疑 COCO/MSMARCO 负结果 | 在 ESCI 和 MSMARCO 上报告 lane density、Stage2 占比、关键路径时间；解释何时端到端收益明显、何时被其它阶段抵消 | B2, B3 |
| AC1: 收益不是来自 recall 下降或索引/聚类变化 | 防止不公平对比 | 固定 centroids/assignments、nprobe、query set、GT、candidate mask；报告 Recall@10/100 和 avg_total_probed/reranked 一致性 | B1 |
| AC2: 不是只优于自研旧格式 | 新颖性边界 | 必须包含官方 RaBitQ compact baseline；如果官方持平或更快，则降级为实现优化 | B1 |

## Experiment Blocks

### B1: 官方 RaBitQ 与新旧布局的公平对比

- Claim tested: C1, AC1, AC2
- Dataset:
  - MUST: `amazon_esci`
  - SHOULD: `msmarco_passage`
  - APPENDIX optional: `coco_100k`
- Compared systems:
  1. `Old VecFetch RaBitQ layout`：优化 RaBitQ 内存之前的方案。
  2. `New vector_bitmajor_tiles/direct compact`：当前优化后方案。
  3. `Official RaBitQ compact packing/kernel`：同一执行器中的官方格式或等价官方 kernel。
  4. 可选二维消融：`new layout + decode-to-scratch`，用于隔离 layout 与 direct compact kernel。
- Fixed setup:
  - 固定同一索引聚类、query、GT、nprobe、topk、bits。
  - topk: `10,100`
  - total_bits: `2,3,4`，主文重点讨论 `4`，`2/3` 作为敏感性。
  - ESCI nprobe: `64,128,256,512`
  - MSMARCO nprobe: `64,256,512`，`1024` 可选。
  - COCO optional nprobe: `64,128,256`
- Metrics:
  - Correctness: `Recall@10`, `Recall@100`, `avg_total_probed`, `avg_reranked`, candidate/mask equality.
  - Latency: avg/p50/p95/p99, QPS, Stage2 ms, final drain ms, rerank ms.
  - Memory/storage: RSS_idle, RSS_peak_query, RSS_delta_query, resident Stage2 code bytes, cluster.clu bytes.
  - Access: Stage2 kernel calls, lanes requested, lanes skipped, lane density, bytes touched/query.
- Success criterion:
  - 新布局相对官方 baseline 在 ESCI 的代表点有稳定收益，且 recall/candidate 不变。
  - MSMARCO 不要求端到端大幅收益，但 Stage2 降低和 Amdahl 解释要闭合。
- Failure interpretation:
  - 若只优于旧布局，不优于官方布局：不能写入贡献段，只放实现/附录。
  - 若只在 ESCI 有收益：可写 workload-aware 子机制，不可写普适默认格式。

### B2: 因果计时闭环和微架构诊断

- Claim tested: C1, C2
- Dataset:
  - MUST: `amazon_esci`
  - SHOULD: `msmarco_passage`
- Compared systems:
  - B1 中表现最关键的 2--3 个系统即可：old、official、新布局。
- Setup:
  - 每个 dataset 选 2 个代表点：
    - 一个中等 recall/低 nprobe 点。
    - 一个高 recall/高 nprobe 点。
  - paired A/B，随机交错运行，至少 5 次；关键结论点 10 次。
  - 绑核、固定 CPU 频率，关闭 fine-grained debug timing 的额外打印开销。
- Metrics:
  - 应用计时：stage1、stage2、decode/direct compute、vector read、payload read、queue wait、final drain。
  - perf counters：cycles、instructions、IPC、L1/LLC misses、branch misses、backend stalls、memory bandwidth。
  - per-query paired delta 和置信区间。
- Success criterion:
  - ESCI 中端到端收益能够被关键路径缩短、cache/memory stall 降低或 overlap 变化解释。
  - MSMARCO 中 Stage2 节省但端到端收益小，也能被 Stage2 占比或关键路径抵消解释。
- Failure interpretation:
  - 若端到端收益无法闭合，只能报告 Stage2 microbenchmark，不可声称端到端收益由新格式导致。

### B3: Survivor-density / 适用区间分析

- Claim tested: C2
- Dataset:
  - MUST: `amazon_esci`
  - SHOULD: `msmarco_passage`
  - APPENDIX optional: `coco_100k`
- Setup:
  - 复用 B1 的 nprobe 网格。
  - 额外记录每 query 的 uncertain count、lane density、Stage2 touched bytes。
  - 如果已有参数可以调节 SafeOut 边界，可只在 ESCI 上补一个 conservative/default/aggressive 小网格。
- Metrics:
  - speedup vs lane density。
  - speedup vs Stage2 ms share。
  - speedup vs recall/topk/nprobe。
- Success criterion:
  - 能给出明确的使用规则：例如低/中 survivor density 时新布局收益明显，高 density 或 Stage2 非关键路径时收益小。
- Failure interpretation:
  - 若没有可解释趋势，新格式只能作为工程实现选择，不作为论文机制。

## Run Order

| Milestone | Goal | Runs | Decision Gate | Cost | Risk |
|---|---|---|---|---|---|
| M0 | 检查公平性 | ESCI bits=4, topk=10/100, nprobe=64 old/official/new smoke | recall、candidate、avg_total_probed 一致 | 0.5 day | official baseline 接入成本 |
| M1 | 最小正文证据 | ESCI bits=2/3/4, nprobe=64/128/256/512, topk=10/100 | 新布局相对 official 不退化且关键点有收益 | 0.5--1.5 day | ESCI 端到端收益归因不闭合 |
| M2 | 第二主数据集 | MSMARCO bits=2/3/4, nprobe=64/256/512, topk=10/100 | Stage2 节省和端到端 Amdahl 解释成立 | 1--2 days | 运行较慢 |
| M3 | 因果诊断 | ESCI + MSMARCO 各选 2 点做 paired/perf | 关键路径闭合 | 1 day | perf 噪声，需要绑核 |
| M4 | Appendix 边界 | COCO bits=2/3/4, nprobe=64/128/256 | 负/边界结果可解释 | 0.5 day | 不进入主 claim |

## Paper Placement

- Main text:
  - 一个小表：ESCI + MSMARCO，old/official/new，在 bits=4、两个 nprobe 点上的 latency、RSS、Stage2 bytes、Recall@10/100。
  - 一个机制图：speedup vs lane density 或 Stage2 share。
- Appendix:
  - bits=2/3 完整表。
  - COCO negative/sanity。
  - perf counter 和重复运行分布。

## Decision Rules

- 若 `new > official` 且因果闭合：作为第二贡献的核心子机制。
- 若 `new ≈ official` 但内存明显更低：作为第二贡献的内存友好实现。
- 若 `new < official` 或收益不可解释：从贡献段删除，只放实现/附录。
- 若 COCO 退化但 ESCI/MSMARCO 可由 survivor density 解释：写成 workload-aware layout，不写成默认普适优化。
