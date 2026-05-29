## Why

当前 MSMARCO `fht_kac_rotator` 验证中，exact coarse select 的 `coarse_select` 约为 `1.15 ms`。双层 IVF routing 在 `m = 2 * nprobe`、`budget = 12 * nprobe` 下已经能把 timing run 的 `coarse_select` 降到约 `0.36 ms`，但仍需要访问约 `3.3K` 个 child centroids，且 recall@10 相比 exact 仍有约 `0.005` 的差距。

为了进一步降低 centroid 层的 coarse select 成本，需要验证一种不依赖全量 centroid scoring、也不依赖大规模 child candidate scoring 的 routing backend。由于仓库已经 vendored Faiss，并且 Faiss 已作为 coarse builder 依赖接入，首版最简单可行方案是在 IVF centroid 层构建一个内存中的 HNSW 图，用图搜索返回 top-`nprobe` clusters。

## What Changes

- 为 `IvfIndex::FindNearestClusters()` 增加一个可选的 HNSW centroid coarse routing backend。
- 使用 Faiss `IndexHNSWFlat` 在已加载的一层 IVF centroids 上在线构建内存图。
- 对 cosine/IP 索引使用 normalized centroids + inner product；对 L2 索引使用原始 centroids + L2。
- 查询时通过 HNSW search 返回 top-`nprobe` centroid labels，并映射到现有 `cluster_ids_`。
- 保留 exact coarse select 作为默认路径和 fallback；保留现有 two-level routing 作为并列实验路径。
- 在 `bench_e2e` 增加 HNSW routing CLI、配置输出和诊断统计，用真实 GT 对比 exact、two-level 和 HNSW。

## Capabilities

### New Capabilities

- `hnsw-coarse-centroid-routing`：面向 IVF centroid 层的可选 HNSW coarse select backend，包含构建、查询、fallback、诊断和 benchmark 验证。

### Modified Capabilities

- `two-level-coarse-routing`：不改变现有行为，仅在 benchmark 对比中作为参考 baseline。

## Impact

- 受影响代码：
  - `IvfIndex` coarse routing 配置、状态、查询分发和统计字段。
  - `SearchConfig` / `SearchStats` routing 配置与诊断输出。
  - `OverlapScheduler` 配置下发。
  - `bench_e2e` CLI、JSON config、pipeline stats 和 benchmark sweep。
  - `test_ivf_index` 与相关 benchmark smoke 测试。
- 受影响行为：
  - 默认 exact routing 不变。
  - HNSW routing 只有显式开启时生效。
  - HNSW 图首版不持久化到磁盘，只在 index open 后或首次 eligible query 前构建并缓存。
- 依赖关系：
  - 复用现有 vendored Faiss CPU 依赖。
  - 不引入新的第三方库。
