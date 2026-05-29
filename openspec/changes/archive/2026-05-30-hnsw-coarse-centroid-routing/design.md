## Context

当前 coarse select 有三条相关路径：

```text
exact:
  score 16384 centroids
  coarse_select ~= 1.15 ms
  recall 安全

two-level:
  score super centroids + selected child centroids
  m = 2 * nprobe, budget = 12 * nprobe
  coarse_select ~= 0.36 ms
  recall@10 比 exact 低约 0.005

proposed HNSW:
  在 16K centroid 层做图搜索
  目标访问几百到一两千 centroid graph nodes
  用更少 centroid scoring 换取接近 exact 的 cluster selection
```

这个 change 只在 centroid 层做 routing，不对 8.8M 原始向量构建图索引。

## Goals / Non-Goals

**Goals:**
- 增加一个可选的 HNSW coarse routing backend。
- 在线构建并缓存一层 centroid 的 HNSW 图。
- 支持当前最重要的 cosine/IP 路径，并保留 L2 支持或安全 fallback。
- 导出 enough benchmark 诊断，能判断 HNSW 是否优于 exact 和 two-level。
- 保持查询结果 downstream 语义：返回 `nprobe` 个 cluster IDs，后续 probe/rerank 不变。

**Non-Goals:**
- 不给原始数据向量构建 HNSW。
- 不修改磁盘 index 格式。
- 不让 HNSW 成为默认 routing。
- 不删除 two-level routing。
- 首版不自研 HNSW/Vamana/NSG。

## Decisions

### Decision: 首版复用 Faiss `IndexHNSWFlat`

使用 Faiss CPU HNSW 作为首版图索引：

```cpp
faiss::IndexHNSWFlat(dim, M, metric)
```

原因：
- 仓库已经 vendored Faiss，并已用于 `faiss_kmeans`。
- 实现成本最低，适合先回答“图路由是否值得”。
- 避免第一版自研 HNSW 的 neighbor selection、层级构建、visited set 和 queue 调优风险。

备选方案：
- 自研 NSW/beam search。暂不采用，因为容易在 recall 和性能上同时不稳定。
- Vamana/DiskANN/NSG。暂不采用，因为 centroid 层只有 16K 级别，工程复杂度不成比例。

### Decision: HNSW 只作为 coarse routing backend

HNSW 图中的节点是 IVF centroids，返回的是 centroid labels。查询结果仍映射到 `cluster_ids_`，后续 cluster probe、Stage1/Stage2、vector fetch、rerank 和 payload 行为完全不变。

```text
query
  -> HNSW over centroids
  -> centroid ids
  -> cluster_ids_
  -> existing probe pipeline
```

### Decision: cosine/IP 使用 normalized centroids

对 `requested_metric == "cosine"` 且 `effective_metric == "ip"` 的索引：
- HNSW build 输入使用 `normalized_centroids_`。
- 查询向量先 normalize。
- Faiss metric 使用 `METRIC_INNER_PRODUCT`。

对非 cosine 的 IP 路径：
- 使用原始 `centroids_`。
- Faiss metric 使用 `METRIC_INNER_PRODUCT`。

对 L2 路径：
- 使用原始 `centroids_`。
- Faiss metric 使用 `METRIC_L2`。
- 如果实现阶段发现当前 Faiss HNSW L2 路径集成不稳定，则 L2 可以先 fallback exact，但必须在 spec/test 中明确。

### Decision: 图构建 lazy + benchmark warmup

HNSW graph 不在 `Open()` 中默认构建。它在以下时机之一构建：
- `bench_e2e` 在正式 query round 前显式 warmup。
- 首次 HNSW routing query 前 lazy build。

构建结果缓存在 `IvfIndex` 内。routing 参数变化时，如果影响 graph build 参数，则清空并重建。

### Decision: 配置参数从简单可控开始

首版 HNSW routing 参数：

```text
--hnsw-coarse-routing 0|1
--hnsw-coarse-m 32
--hnsw-coarse-ef-construction 128
--hnsw-coarse-ef-search 512
```

默认不开启 HNSW。推荐第一轮 sweep：

```text
M: 16, 32
efConstruction: 128, 200
efSearch: 256, 512, 768, 1024
```

### Decision: routing 优先级保持明确

为了避免多个实验 backend 同时开启后行为不清晰，查询分发优先级为：

```text
HNSW enabled and eligible -> HNSW
else two-level enabled and eligible -> two-level
else exact
```

如果 HNSW search 返回结果不足、graph 不可用、metric 不支持或发生异常，则回退 exact，并记录 fallback。

## Risks / Trade-offs

- **风险：HNSW recall 不足** → 通过 `efSearch` sweep 和 exact recall gate 控制。
- **风险：HNSW 随机访问和 heap 开销导致不优于 two-level** → 用 `avg_coarse_select_ms` 与 two-level `~0.36 ms` 比较，未胜出则不推荐默认使用。
- **风险：Faiss HNSW build 成本污染 query latency** → benchmark warmup 构建，build time 单独统计。
- **风险：HNSW labels 与 cluster IDs 映射错误** → 单测覆盖 label 到 `cluster_ids_` 的映射与 exact fallback。
- **风险：已有 two-level change 未归档** → 本 change 作为独立实验 backend，避免继续扩大 two-level change scope。

## Validation Strategy

正式结论必须使用：

```text
--skip-gt 0
--early-stop 0
```

先固定当前最佳 two-level baseline：

```text
--two-level-coarse-routing 1
--two-level-coarse-super-factor 2
--two-level-coarse-budget-factor 12
```

再运行 HNSW sweep。只有当 HNSW recall@10 与 exact 差距不超过 `0.005`，且 `avg_coarse_select_ms` 低于 two-level `~0.36 ms`，才认为 HNSW 真正优于当前方案。
