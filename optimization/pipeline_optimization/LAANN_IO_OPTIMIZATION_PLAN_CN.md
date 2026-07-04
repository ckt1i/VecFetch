# LAANN 对 submit / I/O Pipeline 的启发

时间：2026-06-25T15:17:36+08:00

参考论文：

- Chang Liu, Xiangpeng Hao, Qizheng Zhang, Ju Ren, Zhuo Li. **LAANN: A Low-Latency Graph-Based ANN Search Method with Query-Aware Dynamic Pruning**. arXiv:2606.02784v1, 2026. 链接：https://arxiv.org/abs/2606.02784v1

## 论文要点

LAANN 面向 SSD 上的 graph-based ANN。其关键观察是：

1. SSD I/O 与 CPU 图遍历之间存在并行空间。
2. 低延迟不是简单提交更多 I/O，而是需要让“更可能影响结果”的顶点更早发起 I/O。
3. 当高优先级 I/O 已经发起但尚未返回时，CPU 不应空等，而应处理低优先级但仍可能有收益的工作。
4. 搜索过程需要 query-aware pruning，动态裁剪当前查询下不值得继续扩展的候选。

LAANN 的图搜索场景和我们的 IVF + RaBitQ + rerank pipeline 不同，但 I/O 调度思想可以迁移。

## 映射到当前系统

当前系统的在线查询路径可粗略拆为：

1. coarse routing 选 cluster。
2. Stage1 / Stage2 对量化码做初筛。
3. 对不确定候选 submit 原始向量读取。
4. 原始向量 rerank。
5. assemble final result。

对应到 LAANN：

| LAANN 概念 | 当前系统中的对应项 | 可迁移策略 |
| --- | --- | --- |
| 高优先级 graph vertex | 更接近 frontier 的不确定候选 / 更可能进入 top-k 的向量 | 先 submit，降低关键路径等待 |
| 低优先级 vertex | 排名靠后的 uncertain 候选 / SafeOut 边界附近候选 | I/O 等待期间继续处理，避免 CPU 空转 |
| dynamic pruning | SafeOut frontier / RabitQ bound | pending read 可以随 frontier 更新而降级 |
| I/O 与计算 overlap | Stage2 scan、候选整理、rerank read | submit 后继续扫描或整理下一批 |

## 后续 I/O 优化方案

### 1. Priority submit queue

为不确定候选计算轻量 priority：

- `priority = lower_bound_distance - current_frontier`
- 或使用当前估计距离、cluster 顺序、Stage2 bound 组合。

提交策略：

- 每批优先提交最可能进入 top-k 的候选。
- 低优先级候选暂存到 backlog。
- 当高优先级 I/O outstanding 后，CPU 继续处理 backlog 或下一批 cluster。

预期收益：

- 降低 final drain。
- 降低 submit 到 rerank 的关键路径等待。

风险：

- priority 维护和排序不能超过 I/O 等待节省。
- 需要先用 bounded heap / bucket，避免全量排序。

### 2. Address-local batching

在不破坏 priority 的前提下，对同一优先级窗口内候选按 `data.dat` 地址排序或 coalesce。

策略：

- 先按 priority 分 bucket。
- 每个 bucket 内按地址排序。
- 限制窗口大小，例如 32/64/128 candidates。

预期收益：

- 减少随机读放大。
- 改善 NVMe/page cache 行为。

风险：

- 地址排序可能延迟最高优先级候选，必须只在小窗口内做。

### 3. Pending read 降级

当 frontier 在 Stage2/部分 rerank 后明显收紧时，部分尚未 submit 的 backlog 可以重新判定。

策略：

- 已经 submit 的 I/O 默认不取消，避免复杂性。
- 未 submit 的低优先级 backlog 在每轮 frontier 更新后重新过滤。

预期收益：

- 降低无用 `data.dat` 读取。

风险：

- 不能影响 recall；只允许使用安全 bound。

### 4. Tail timing 拆分

当前 MSMARCO `unaccounted` 约 `9%`，后续优化前应继续拆：

- submit queue build
- io_submit / io_uring_enter
- wait completion
- completion handling
- collector finalize
- final result assembly

这些计时会决定优先优化 submit、wait、collector 还是 assembly。

## 实验设计

每轮优化至少报告：

- `avg_query_time_ms`
- QPS
- recall@100
- `avg_probe_submit_ms`
- `avg_final_drain_ms`
- `avg_rerank_compute_ms`
- `avg_search_unaccounted_ms`
- `avg_candidates_reranked`
- `avg_vec_only_read_requests`
- `avg_all_read_requests`

默认先在 ESCI/MSMARCO 上测 `total_bits=4/ex_bits=3`，若有效再扩展到 `ex_bits=1,2,3`。

