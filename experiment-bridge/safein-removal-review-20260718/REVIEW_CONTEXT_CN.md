# RecordGate SafeIn 去留研究评审上下文

日期：2026-07-18  
目的：判断是否应将 SafeIn 从论文主方法、默认执行路径和正式实验主线中整体移除，并审核移除后的新叙事。

## 1. 当前待审核的新叙事

传统向量系统通常把 ANN/vector rerank 与后端 record store 分开优化，以 id 作为边界。候选生成之后，系统按固定 rerank budget 读取 raw vectors，再从独立 record backend 读取 final payload，缺少 candidate evidence、物理读取计划和 record materialization 的协同。

拟议的新 RecordGate 主线：

1. 使用 RaBitQ 概率误差界和 evolving exact frontier，动态跳过不再可能进入 top-k 的候选，只对剩余候选执行 raw-vector verification。
2. 在 IVF probing 期间异步发起 mandatory vector reads；把同 tile、地址有序的读取建模为固定顺序连续区间分段，按 `(requests,physical bytes)` 字典序优化，并用一维 endpoint dominance 把 exact DP 从 `O(n^2)` 降为 `O(n log n)`。
3. 使用 size-adaptive span-reusable record layout：小 payload/prefix 与 raw vector 共置，大 payload external。mandatory vector span 自然覆盖的 inline payload 形成 reusable view，top-k sealed 后只补读缺失 payload。

SafeIn-aware span credit、eager full-record prefetch、cold-prefix prefetch 和 tail extension 拟从主方法删除。

## 2. 理论不对称

令候选 `i` 的概率距离区间为 `[L_i,U_i]`。

### SafeOut

在线维护 conservative kth upper frontier `T_t`。当前实现保留已见候选中最小的 k 个 upper bounds，因此随扫描推进：

\[
T_{t+1}\le T_t.
\]

若 `L_i>T_t`，则在配置的 bound/confidence event 成立时，候选不可能进入当前 top-k。由于 frontier 只会收紧，一旦 SafeOut，后续仍满足 `L_i>T_{t'}`。这是单调的 pruning certificate。

注意：当前 epsilon/bound 是概率与校准口径，不是无条件确定性界。因此论文只能写 `confidence-budgeted` 或 `under the configured bound policy`，不能写“绝不会误判”。

### SafeIn

SafeIn 若使用当前 kth lower/reference frontier `S_t`，条件为 `U_i<S_t`。随着更多优质候选出现，`S_t` 也可能下降，所以早期条件不保证对未来成立。它最多证明候选在“当前已见集合”中很强，不能保证最终 top-k membership。

要获得对完整候选集的 membership certificate，必须等到扫描完成后再比较 `U_i` 与完整集合的 kth lower bound；但此时已经失去 early payload prefetch 的 overlap 价值。因此 SafeIn 存在结构性的 certainty–earliness tension，而 SafeOut 没有同方向的问题。

这并不意味着每个早期 SafeIn 都必然失效，只表示该分类不是跨扫描单调的最终结果证书。

## 3. 最新实验事实

### SafeOut

- NoSafeOut 保持或略提高 recall，但 ESCI/MSMARCO latency 分别约慢 `37.3x/65.8x`；reranked candidates 与 read bytes 约增加两个数量级。
- 该结果强力支持 bound-guided verification reduction。
- 但 NoSafeOut 不是固定 rerank-depth baseline，不能单独证明“固定 rerank budget 缺乏理论依据且误差大”。

### Traditional eager SafeIn full-record prefetch

严格 `GE,rho=0,reuse=0,tail=0` paired ablation：

| Dataset | QPS delta | Positive reps |
|---|---:|---:|
| ESCI | -2.93% | 0/5 |
| COCO | -0.46% | 0/5 |
| MSMARCO | -1.26% | 0/5 |
| Vox | -1.56% | 0/5 |

20/20 paired repetitions 为负；采用最低 NoSafeIn fixed baseline 后仍为 20/20 负。

### External/cold payload SafeIn

- ESCI/Vox、topk 10/100、cold prefix 4/8/16/64 KiB 未形成稳定加速。
- 最终 Vox topk100 count8 中 Combined `-0.85%`、NoCombine `-0.80%`。
- 约 57%--68% prefetched bytes 未被 final top-k 使用。
- 结论：机制可行但默认关闭，不进入论文性能主张。

### SafeIn-aware SE span admission

64 KiB q500x5：SE/GE 在 ESCI/COCO/MSM/Vox 分别为 `-0.485%/+0.154%/-0.709%/-0.148%`。16/32 KiB tile/read-bound sweep 也未找到可复现胜点；COCO@32 KiB q500x5 为 `-0.016%`。

SE 能减少部分 requests，但增加 bytes，只形成 request/byte Pareto 变化，不支持 throughput claim。

### Natural span payload reuse

GEReuse 相对 NoPayloadReuse：

- ESCI/MSM total requests `-8.012%/-10.441%`；total bytes `-1.228%/-1.143%`；
- QPS 为 `-0.162%/+0.421%`，未稳定越过噪声；
- COCO/Vox 当前 GE 规划没有 reusable payload view。

因此 natural reuse 可支撑“减少 final materialization I/O”的机制 claim，但不能单独声称普遍提高 QPS。

### Span 与 exact planner

- NoSpan→GV：ESCI/MSM requests `-55.20%/-68.05%`，QPS `+1.82%/+4.81%`。
- GE→GV：requests 进一步 `-1.49%/-0.22%`，QPS `-0.89%/-0.62%`；planner约 `0.06 ms/query`。
- Exact partitions 与独立 `O(n^2)` oracle 匹配；43/43 tests 通过。
- 结论：主要性能收益来自 bounded span；exact 提供模型内最优性和少量请求收益，不是 QPS winner。

### Pipeline

- Full vs synchronous NewNoPipeline 有显著差距，但控制同时改变 overlap、async batching、payload reuse、tail 和 SafeIn，不能把全部差距归因于 CPU/I/O overlap。
- COCO 无 SafeIn/reuse/span 命中仍有 `+2.31%/+6.22%`，说明 async/batching 路径有独立价值。
- 需要 `NoOverlapAsyncFinal`：probe 后才读，但 raw vectors/final payload 仍用相同 io_uring batch path，才能隔离跨-probe overlap。
- 诊断表明 Vox 主要受 final payload plan/submit/assembly 限制，MSM 主要受 mandatory vector PrepRead/SQE/CQE 软件路径限制；optional SafeIn 的机会远小于这些成本。

## 4. 当前证据中的冲突与解释

旧 P0/P1 sweep 曾在 Vox/MSM 个别点观察到约 0.4%--2.2% SafeIn/full-stack 增益，但处理同时包含 span reuse、tail、layout、cache state 与 SafeIn，不能隔离 SafeIn。最新严格 eager、cold-prefix、SE/GE 和 tile-bound 实验均不支持 SafeIn 作为稳定独立机制。

因此，“删除 SafeIn”不是忽略正结果，而是采用更严格的结果归因：保留 natural mandatory-span reuse，删除 speculative classification-driven payload I/O。

## 5. 需要 reviewer 回答的问题

1. SafeIn 是否应从论文主方法、默认执行路径和主实验完全移除？代码是否只需默认关闭而不必立即物理删除？
2. 上述 SafeOut/SafeIn 单调性论证是否严谨？哪些表述会过度声称？
3. 移除 SafeIn 后，三点新叙事是否足够构成数据库/系统论文贡献？应合并成两条还是保留三条？
4. “固定 rerank budget 缺乏理论依据、误差大”是否需要额外 baseline？最安全表述是什么？
5. pipeline、exact planner、adaptive layout 各需要哪些最小补实验？哪些已有结果可以复用？
6. 请给出 results-to-claims matrix、最小高优先级实验包和 section-level paper outline。

## 6. 时间与范围

- 剩余时间少于两周。
- 不新增大型参数 sweep、learned cost、跨 query planning 或新数据格式大改。
- 优先复用现有 final binary、索引、GT、queries 和已有 paired results。
