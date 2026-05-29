## 1. 配置与接口

- [x] 1.1 在 `SearchConfig` 增加 HNSW coarse routing 配置：enable、M、efConstruction、efSearch。
- [x] 1.2 在 `SearchStats` 增加 HNSW 诊断字段：routing mode、graph build ms、M、efSearch、exact fallback、返回/访问节点统计。
- [x] 1.3 在 `bench_e2e` 增加 CLI：`--hnsw-coarse-routing`、`--hnsw-coarse-m`、`--hnsw-coarse-ef-construction`、`--hnsw-coarse-ef-search`。
- [x] 1.4 在 JSON config、pipeline stats 和 per-query sample 中输出 HNSW 配置与诊断字段。

## 2. HNSW Graph 数据模型与构建

- [x] 2.1 在 `IvfIndex` 内增加 HNSW coarse graph 状态，保存 Faiss `IndexHNSWFlat`、build metadata 和 readiness state。
- [x] 2.2 实现 `SetHnswCoarseRouting(...)`，当 build 参数或 metric 变化时清空 cached graph。
- [x] 2.3 实现 `PrepareHnswCoarseRouting()`，用于 benchmark warmup 和首次 query 前 lazy build。
- [x] 2.4 cosine/IP 索引用 `normalized_centroids_` 构建 inner-product HNSW；L2/IP 非 cosine 按 metric 选择 Faiss metric 或 fallback exact。
- [x] 2.5 graph build 失败时记录 fallback，不影响 exact/two-level 路径。

## 3. HNSW 查询路径

- [x] 3.1 新增 `FindNearestClustersHnsw(query, nprobe)` helper。
- [x] 3.2 在 `FindNearestClusters()` 中按 `HNSW -> two-level -> exact` 分发。
- [x] 3.3 HNSW search 返回 labels 后映射为现有 `cluster_ids_`，保持 downstream probe order 语义。
- [x] 3.4 查询结果不足、label 越界、graph 未 ready 或 metric 不支持时回退 exact。
- [x] 3.5 HNSW diagnostic 不得混入 exact-overlap timing；如需 overlap，单独 diagnostic run。

## 4. Benchmark 与验证

- [x] 4.1 `bench_e2e` 在正式 query round 前调用 HNSW warmup，避免首 query build 污染 steady-state latency。
- [x] 4.2 跑 exact baseline：真实 GT、`--early-stop 0`、当前 MSMARCO `fht_kac_rotator` index。
- [x] 4.3 跑当前最佳 two-level baseline：`--two-level-coarse-super-factor 2 --two-level-coarse-budget-factor 12`。
- [x] 4.4 跑 HNSW sweep：`M=16/32`、`efConstruction=128/200`、`efSearch=256/512/768/1024`。
- [ ] 4.5 对最佳 HNSW 配置跑 perf，确认 coarse select 采样占比下降，且主要热点没有转移到 Faiss queue/visited set。

## 5. 测试

- [x] 5.1 单测覆盖 HNSW disabled 时 exact 路径完全不变。
- [x] 5.2 单测覆盖 HNSW graph build 可重复调用，ready 后不重建。
- [x] 5.3 单测覆盖 cosine/IP 小索引下 HNSW 返回有效 cluster IDs，且 `routing_mode == 2`。
- [x] 5.4 单测覆盖 HNSW fallback：graph 不可用、结果不足、unsupported metric。
- [x] 5.5 benchmark smoke 测试覆盖新 CLI 参数解析和 JSON schema。

## 6. Acceptance Criteria

- [x] 6.1 correctness gate：HNSW recall@10 与 exact 差距不超过 `0.005`。
- [x] 6.2 performance gate：HNSW `avg_query_time_ms` 低于 exact。
- [ ] 6.3 competitive gate：HNSW `avg_coarse_select_ms` 低于当前 two-level `~0.36 ms`，否则不能声称优于当前 two-level 方案。
- [x] 6.4 如果 HNSW 未通过 competitive gate，报告其只作为探索结果保留，并建议继续优化 exact/two-level 或考虑更强图 routing。
