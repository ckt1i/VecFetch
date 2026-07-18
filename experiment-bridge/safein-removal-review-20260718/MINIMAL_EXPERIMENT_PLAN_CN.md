# SafeIn 移除后的两周最小实验计划

日期：2026-07-18

## 总原则

- 不再寻找 SafeIn 的胜点；已有 eager、external-cold 和 SE/GE 结果足以作设计淘汰证据。
- 只补会改变论文可写主张的对照。
- 复用 final binary、现有索引、GT、query trace、线程数和 cache/cold-start 口径。
- 主结果至少报告 QPS、p50/p99、与实际 top-k 配置对齐并预先冻结的 recall 主指标、read requests、physical bytes、useful bytes 和 planner CPU time；fixed-R 使用同一 recall 口径和置信区间。

## P0：必须完成的三项

### E1. Fixed-depth rerank frontier

**目的**：验证“动态 bound-guided verification 优于固定 rerank budget”的新增比较主张。

**数据集**：ESCI、MSMARCO；若一天内可完成，再加 Vox。

**方案**：

1. RecordGate SafeOut（其余路径固定为最终默认配置）。
2. Fixed-R baseline，选择 3--5 个 R，使其覆盖低于、接近和高于 RecordGate recall 的区间。
3. 可选 NoSafeOut 仅作上界，不替代 Fixed-R。

**胜出条件**：在与论文最终 top-k 配置一致的主 recall 指标下（若冻结指标为 Recall@10/Recall@50，则两者均报告），matched-recall 置信区间内 RecordGate 的 raw-vector requests/bytes 和 latency 显著更低；或者给出更优的 recall--verification-work Pareto frontier。

**若未胜出**：删除与固定 budget 的优越性比较，只保留“动态、query-dependent work allocation”机制描述和 NoSafeOut 消融。

### E2. `Combined/NoCombine × NoSpan/GE` 两因素交互

**目的**：验证第二条贡献确实是 layout--span 协同，而不是普通读合并与常规 inline placement 的松散叠加。

**数据集**：ESCI、MSMARCO。

**方案**：

1. `Combined + NoSpan`；
2. `Combined + GE`；
3. `NoCombine + NoSpan`；
4. `NoCombine + GE`。

四个 cell 固定 candidate set、SafeOut、reader、cache、top-k 和 SafeIn-free 配置。报告 vector/payload requests、physical bytes、per-span real-vector-byte amplification、eligible/covered/registered/consumed inline bytes，以及 final missing-payload requests/bytes。

**胜出条件**：GE 在两种 layout 下均减少 vector requests，但只有 Combined+GE 相对自身 NoSpan 产生合法、被 final materializer 消费的 payload views，并减少 final missing-payload work。QPS 不是判断 interaction 成立的唯一条件。

**若未胜出**：第二条贡献降级为 bounded read coalescing；layout 只作为实现细节，不再声称 span-reusable record substrate。

### E3. SafeIn-free correctness/configuration audit

该项不是大规模性能实验，但必须在 paper artifact 冻结前完成：

1. telemetry 证明 eager SafeIn、cold-prefix、SE credit、tail 均关闭，natural span reuse 仍开启；
2. guarantee 限定于 ANN/IVF candidate set，candidate-generation recall 单列；
3. 给出 query-level joint bound/failure-budget accounting；若只有 marginal calibration，则降为 empirical confidence policy；
4. 审计 strict `L_i>T_t`、tie handling、至少 k 个 witnesses 与 fallback；
5. 回归测试证明关闭 SafeIn 不改变 candidate membership 与 exact verification 语义。

## 可直接复用的三组证据

### E4. Span planner

- 复用 NoSpan→GV 的 ESCI/MSMARCO paired 结果说明 request reduction 和端到端效果。
- 复用 GE→GV 说明 exact 与 greedy 的取舍：更少 requests、约 0.06 ms/query 规划成本、未必更高 QPS。
- 复用 43/43 tests 和 O(n^2) oracle 证明模型内 exactness。

### E5. Natural payload reuse

- 复用 GEReuse/NoPayloadReuse 的 ESCI/MSMARCO I/O 结果。
- 先审计 counters；若没有 eligible/covered/registered/consumed 的完整计数，就做一次受控复跑，不能把缺失证据当作离线导出。
- COCO/Vox 作为 `zero-coverage` workload 如实报告，证明该机制是条件性机会而非普遍收益。

### E6. SafeOut

- 复用 NoSafeOut 的 latency、rerank count、requests、bytes 和 recall。
- 增加理论限定：结果依赖配置的概率界/置信策略，不能写无条件零误差。

## P1：有余力再做

### E7. 公平的 pipeline overlap 消融

仅当摘要/贡献坚持声称跨 probe overlap 有独立加速时升级为 P0。

1. `Pipeline`：probing 期间发起 currently admitted vector span reads。
2. `NoOverlapAsyncFinal`：probe 完成后才发起完全相同的 reads，保留同一 io_uring、batch size、queue depth、layout、planner、reuse 和 final payload path。

通过条件：paired 5 repeats 中大多数同向，QPS/latency 超出噪声，同时 requests 和 bytes 基本不变。未完成时只把 async issuance 写成实现机制。

### E8. Layout eligibility sensitivity

只做 2--3 个离散 inline/prefix 阈值，不做大 sweep。目标是展示 format 如何改变 span-covered payload 的机会，而不是调出最佳 QPS。

### E9. End-to-end interaction table

按以下累加顺序复用或补齐：

1. Static/NoSafeOut verification baseline；
2. + SafeOut；
3. + bounded span；
4. + cross-probe overlap；
5. + inline payload reuse。

该表必须保持同一 planner、layout 和 async path，避免再次形成 P0/P1 full-stack 混淆。

## 明确不做

- SafeIn threshold、credit、cold prefix、tail count 的进一步 sweep。
- learned I/O cost model、跨 query batching/plan、全局装箱或一般图优化。
- 为证明 exact QPS 胜过 greedy 而反复调参。
- 在两周内物理删除全部 SafeIn 代码；先把默认路径和论文路径清理干净。

## 预计日程

| 时间 | 工作 |
|---|---|
| Day 1--2 | 固化 SafeIn-free config/correctness contract；实现/核对 Fixed-R 与四格 interaction 开关 |
| Day 3--5 | 跑 E1、E2，统一 cache/warmup、A/B 交替与 paired repeats |
| Day 6 | 汇总 E4--E6 已有数据；审计或补跑 natural-reuse coverage counters |
| Day 7--8 | 补失败重跑；有余力再做 E7；冻结实验 |
| Day 9--10 | 更新主表、消融表、方法与讨论 |
| Day 11--12 | claim audit、复现实验脚本与 artifact 整理 |
