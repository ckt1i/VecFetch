## Context

当前 DiskANN baseline 有两个关键正确性约束。第一，旧版 recall helper 只要某个 query 命中了任意一个 ground-truth item，就把该 query 记成完全正确，这会显著抬高 `recall@10`；新的评测必须改为 top-k overlap 平均值。第二，Python `diskannpy.StaticDiskIndex` 路径在现有 COCO100k disk index 上已经出现重复 top-k ID，因此这次 change 中唯一可接受的 DiskANN 正式测量路径只能是 C++ CLI 输出。

COCO100k 已经有可用的 DiskANN 索引，也已经验证过一条 C++ CLI smoke 路径。MSMARCO 已经具备 formal baseline 资产、ground truth 和 FlatStor payload 存储，但还缺少 DiskANN 建索引与 C++ CLI 评测支持。这次对比的目标也不只是纯向量搜索，而是与 VecFetch 对齐的计时口径：`search(DiskANN) + read_payload(FlatStor)`。

## Goals / Non-Goals

**Goals:**

- 在 COCO100k 和 MSMARCO 上产出修正后的 DiskANN Pareto 点，尽量覆盖 `recall@10` 约 `0.85` 到 `0.995` 的区间。
- 先跑 COCO100k，优先复用现有 COCO 索引；只有在搜索参数扫描无法覆盖目标 recall 区间时，才新增 COCO 索引构建。
- 至少构建一个 MSMARCO DiskANN 索引，并记录足够的元数据以复现实验输入与索引参数。
- 在统一输出 schema 下记录 DiskANN 搜索延迟、FlatStor payload 读取延迟、合并延迟、p50/p95/p99、duplicate rate，以及修正 recall。
- 产出可直接与 VecFetch 对比的 Pareto CSV 和目标 operating point 汇总。

**Non-Goals:**

- 不把 `diskannpy` 的结果作为正式 benchmark 数据。
- 不把 DiskANN 建索引时间计入在线查询延迟。
- 不修改 VecFetch 的搜索语义或 payload 存储格式。
- 不在 Pareto 已经收敛后继续做无边界的全参数穷举。
- 不替换现有 benchmark 基础设施，而是在外部 DiskANN baseline 周围补充脚本和结果工件。

## Decisions

1. 所有正式 DiskANN 搜索结果统一走 C++ DiskANN CLI。

   原因是 C++ 路径可以避开 Python 绑定上的重复 ID 问题，而且它本身就是 DiskANN 支持的 disk-search 可执行路径。备选方案包括继续使用 Python 绑定后处理去重，或者只在低 recall 点上用 Python 绑定，但这两种方式都会让最终 baseline 依赖于并非 DiskANN CLI 原生产生的行为。

2. 修正后的 overlap recall 是唯一正式 recall 指标。

   对每个 query，计算 `|pred_topk ∩ gt_topk| / k`，忽略无效 sentinel ID，然后在 query 维度求平均。输出里可以保留兼容字段，但旧的 hit-rate 式 recall 不允许再用于 Pareto 点选择。

3. COCO100k 先扫搜索参数，再决定是否构建新索引。

   第一轮 COCO sweep 先使用已验证的 `R=64,L_build=100` 索引，扫描 `L_search` 和 `beam_width`。如果低 recall 区域仍然偏高，再退到现有 `R=32,L_build=50` 索引。只有两个现有索引都无法覆盖目标低 recall 区间时，才额外构建一个更弱的 COCO 索引。

4. MSMARCO 索引按缺口自适应构建。

   首先构建一个中等强度的 MSMARCO disk index，例如 `R=32,L_build=50`，并为 768 维 embedding 选择合适的 disk-PQ byte 设置。先扫描 `L_search` 和 `beam_width`；如果高 recall 区间够不到，再补一个更强的索引，比如 `R=48/64,L_build=80/100` 加更大的 PQ 预算。如果所有点都太高 recall、太慢，不利于对比，则再补一个更弱的索引，比如 `R=24,L_build=40` 加更小的 PQ 预算。

5. 每个索引目录旁边都保留 manifest。

   每个索引目录都应记录 dataset 标识、base/query 文件路径、ground-truth 路径、向量维度、metric、`R`、`L_build`、PQ 设置、DiskANN 可执行路径、构建命令、构建时间戳，以及 companion 文件状态。这样即使后面构建了多个索引，也能保持 Pareto 结果可复现。

6. FlatStor payload 读取单独计时，再显式合并。

   DiskANN 搜索先输出 top-k ID，然后单独执行 FlatStor 读取并记录 payload-read latency。最终用于对比的指标是 `diskann_search_ms + flatstor_payload_ms`，同时保留两部分组件时间，便于后续归因。

7. 对无效点显式拒收，不做静默修补。

   只要某个点存在单 query 内重复结果 ID、可用 ID 数量少于 `k`、query 与 ground truth 数量不匹配、payload 记录缺失或 CLI 解析失败，就应视为无效点。无效行可以写出，但必须带原因标记，且不能进入 Pareto 选择。

## Risks / Trade-offs

- [MSMARCO 建索引成本高] -> 先从一个中等强度索引开始，跑有界 sweep，只有在确认 frontier 缺高 recall 或低 recall 时才构建第二个索引。
- [C++ CLI 计时可能包含进程启动开销] -> 优先采用 DiskANN 输出中的 query 级延迟字段，同时保留 wall-clock latency 作为诊断字段。
- [FlatStor payload 计时方式可能与 VecFetch batching 有差异] -> 记录 fetch count、payload bytes 和逐 query payload timing，使对比口径透明。
- [Recall 覆盖可能达不到所有目标阈值] -> 输出最近的有效 operating point，并把未达到的阈值显式标记为 unreached，而不是做不受支持的插值。
- [现有 COCO companion 文件可能过期或缺失] -> 在正式 sweep 前先做 companion 文件校验，必要时根据 index metadata 和 base vectors 重新生成。
- [结果网格过大可能消耗时间却不提升 frontier 质量] -> 先做粗扫，再围绕 threshold crossing 做定向细化。
