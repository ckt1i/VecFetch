## Context

当前 `IvfIndex::FindNearestClusters()` 采用 exact coarse select：对所有一层 centroid 打分，再选出 top-`nprobe`。这个方案简单且对 recall 更安全，但当 `nlist` 很大时，每个查询的打分成本都会正比于 `nlist * dim`。最近在 MSMARCO resident/full-preload 场景下，随着通过 epsilon 调整降低了 Stage2 uncertain 规模，coarse select 已经成为明显的 CPU 开销项。

现有实现已经在 `effective_metric=ip` 路径上具备优化过的 packed centroid scoring，因此这次 change 的目标是减少算法层面的工作量，而不是继续重写一版全量 centroid kernel。

## Goals / Non-Goals

**Goals:**
- 为大 `nlist` IVF 搜索增加一个可选的双层 coarse routing backend。
- 基于现有一层 centroids 构建并缓存一个 super-centroid 层次结构。
- 默认使用 `m = ceil(nlist / 128)` 作为 super cluster 数量。
- 使用 `candidate_budget = 8 * nprobe` 限制需要实际打分的一层 child centroid 数量。
- 在选中的 child candidate 集合内部，保留真实的一层 centroid 打分。
- 保留 exact coarse select 作为 fallback 和验证参考路径。
- 导出足够的 benchmark 元数据，用于归因速度和 recall 的变化。

**Non-Goals:**
- 首版实现不修改磁盘上的 index 格式。
- 不替换 IVF probe 逻辑或 cluster 数据布局。
- 本次不引入 HNSW/graph routing。
- 不修改 Stage1、Stage2、probe submit、rerank 或 payload 行为。
- 在 recall 完成验证前，不把双层 routing 设为生产默认路径。

## Decisions

### Decision: 层次结构只构建一次，并缓存在 `IvfIndex` 内

层次结构应在 index open 之后构建，或者在首次双层查询前按需 lazy build，随后在多个查询间复用。它不得按 query 重建。

原因：
- 如果每个 query 都重新聚类，开销会主导整体查询延迟。
- 这个层次结构只依赖已加载的 centroids 和 routing 参数。
- 这样在线查询路径只剩 super score、child score 和 selection。

备选方案：
- 每个 query 动态聚类。否决，因为这会把一个亚毫秒级 coarse scoring 问题，变成明显更重的在线聚类负担。

### Decision: 使用 `m = ceil(nlist / 128)` 作为默认 super cluster 数

默认 super-cluster 数量从一层 `nlist` 推导：

```text
m = ceil(nlist / 128)
```

这意味着平均每个 super cluster 大约覆盖 128 个一层 child centroid。

原因：
- super scoring 的规模较小。
- 对不同数据集给出简单且可复现的默认值。
- 这与当前要求的策略一致，并且是较保守的首版实现。

备选方案：
- `m = ceil(nlist / 64)`。它能提供更细粒度的 routing，可能改善 recall，但会增加 super score 的工作量，不是本次要求的默认值。

### Decision: 按 child candidate budget 选择 super clusters

查询路径使用：

```text
candidate_budget = 8 * nprobe
super_probe = ceil(candidate_budget / average_children_per_super)
```

实现中需要把 `super_probe` clamp 到 `[1, m]`，并尽可能保证收集到足够的 child candidates，以便选出 top-`nprobe`。

原因：
- 用 budget 控制可以让一层 centroid 打分量更可预测。
- 比起“固定探测一部分 super cluster”，这种方式更有实际意义。
- 在 `m = ceil(nlist / 128)` 的默认设置下，大致有：

```text
average_children_per_super ~= 128
super_probe ~= ceil(8 * nprobe / 128) = ceil(nprobe / 16)
```

对于 `nlist=16384` 且 `nprobe=256`：

```text
m = 128
candidate_budget = 2048
super_probe = 16
```

### Decision: 用真实的一层 centroid score 对选中的 child centroids 排序

super-centroid score 只用于决定哪些 child centroids 有资格进入候选集合。最终 top-`nprobe` 的排序必须基于原始一层 centroids 的真实分数来完成。

原因：
- 把近似限制在 candidate generation 这一步。
- 在选中的 child candidate 集合内，尽可能保留现有 downstream probe order 约定。

### Decision: 保留 exact routing 作为 fallback 和参考路径

exact 路径必须继续可用，并在以下情况使用：
- 双层 routing 被关闭；
- `nlist <= threshold`；
- hierarchy 不可用或无效；
- 收集到的 child candidate 数量小于 `nprobe`；
- 首版实现尚未支持当前 metric/path。

原因：
- 双层 routing 可能会漏掉包含 exact top-`nprobe` centroids 的 super cluster。
- exact fallback 对正确性、调试和 A/B 对比都是必须的。

### Decision: 首版只使用内存中的 hierarchy

hierarchy 直接从已加载的 centroids 派生，并缓存在内存中。把 hierarchy 持久化到磁盘不在这次 change 的范围内。

原因：
- 避免引入 index 格式迁移。
- 让实现聚焦在 recall/latency tradeoff 的验证上。

### Decision: 二层构建器复用 `SuperKMeans`

二层 hierarchy 的 super cluster 不能用随机投影排序分桶构建。当前验证显示这种近似分桶对 exact top-`nprobe` centroid 的覆盖率过低，MSMARCO `nprobe=256` 时 overlap 只有约 `72/256`，会直接导致 recall 明显下降。

因此 builder 改为复用已有 `skmeans::SuperKMeans` 对一层 IVF centroids 做离线内存重聚类：

```text
n_super = ceil(nlist / 128)
candidate_budget = 8 * nprobe
```

固定训练参数：

```text
iters=10
seed=42
sampling_fraction=1.0
max_points_per_cluster=256
n_threads=0
early_termination=true
tol=1e-4
verbose=false
```

对 `requested_metric == "cosine"` 且 `effective_metric == "ip"` 的索引，使用 `normalized_centroids_` 作为 SuperKMeans 输入，并设置 `angular=true`。非 cosine 路径使用原始 `centroids_`，并设置 `angular=false`。

SuperKMeans 只负责生成 super centroids 和一层 centroid 到 super id 的 assignment；查询时仍保留现有行为：先 score super centroids，再展开 child candidates，最后用真实一层 centroid score 重新排序。

### Decision: 最终验证必须关闭 early stop 并线性执行

two-level routing 的验证必须使用真实 GT，并且显式设置：

```text
--skip-gt 0
--early-stop 0
```

exact baseline、two-level diagnostic 和 two-level timing 必须串行执行，不能并行跑。原因是 coarse routing 的收益和 recall 变化需要在固定 probe 数下归因；early stop 会改变实际 probe 行为，并行运行会引入 CPU 竞争，影响毫秒级 latency 判断。

## Risks / Trade-offs

- **风险：因为 super-cluster miss 导致 recall 下降** → 缓解方式：保留 exact fallback，导出 exact-overlap 诊断，并且 benchmark 只接受真实 GT recall。
- **风险：启动或首个 query 的构建时间增加** → 缓解方式：单独统计 hierarchy build time，不把它混入 steady-state query latency。
- **风险：`m = ceil(nlist / 128)` 对某些数据集来说过粗** → 缓解方式：暴露 super count 和 budget factor 的 override 参数，同时保留当前默认值。
- **风险：child centroid scoring 变得 cache 不友好** → 缓解方式：第一版先以 correctness 为主完成 selected-child scoring；如果 profiling 证明它成为热点，再继续优化布局。
- **风险：cosine 和 L2 路径需要单独处理** → 缓解方式：首版可以先支持当前最热的 `effective_metric=ip` 路径，不支持的模式直接 fallback。
