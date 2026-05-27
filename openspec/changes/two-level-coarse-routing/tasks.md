## 1. Routing 配置与统计

- [x] 1.1 在 `SearchConfig` 中增加双层 coarse routing 配置字段：routing mode、threshold、super count override、budget factor，以及 enable flag/defaults。
- [x] 1.2 为 `bench_e2e` 增加双层 routing 的 CLI 参数，并把选中的参数值写入 JSON config 输出。
- [x] 1.3 在 `SearchStats` 中增加 routing 诊断字段：实际使用的 routing mode、super count、super probes、child candidates scored、hierarchy build time 和 exact fallback count。
- [x] 1.4 在双层 routing 关闭时，保持现有 coarse timing 字段和 exact routing 行为不变。

## 2. Hierarchy 数据模型与构建器

- [x] 2.1 在 `IvfIndex` 内增加 `HierarchicalCoarseIndex` 结构，包含 super centroids、child offsets、child centroid ids、packed super layout、build metadata 和 readiness state。
- [x] 2.2 实现默认参数推导：`super_count = ceil(nlist / 128)` 和 `candidate_budget = 8 * nprobe`，并做好安全 clamp。
- [x] 2.3 实现一次性的内存构建器：把一层 centroids 聚成 super clusters，并构建 `super -> child centroid ids` 映射。
- [x] 2.4 为 super centroids 构建 packed layout，使 super scoring 路径可以复用现有 packed IP score dispatch。
- [x] 2.5 确保 hierarchy 在 index open 后或首次满足条件的双层查询前 lazy build，并且除非 routing 参数变化，否则不按 query 重建。

## 3. 双层查询路径

- [x] 3.1 把当前 exact 实现拆成独立 helper，使 `FindNearestClusters()` 可以清晰地在 exact 和双层路径之间分发。
- [x] 3.2 基于由 budget 推导出的 super probe 数，完成 super-centroid scoring 和 top-super selection。
- [x] 3.3 将选中的 super clusters 展开为去重后的 child centroid candidate 列表。
- [x] 3.4 对选中的 child centroids 基于原始一层 centroids 进行打分，并按 child score 选出最终 top-`nprobe`。
- [x] 3.5 当 hierarchy 无效、当前 metric/path 不受支持，或者收集到的有效 child candidates 少于 `nprobe` 时，回退到 exact routing。
- [x] 3.6 保持返回 child centroids 时的 cluster ID 映射和 downstream probe order 语义不变。

## 4. Benchmark 与诊断

- [x] 4.1 在 aggregate JSON 和 per-query sample 中导出 routing 元数据：routing mode、threshold、super count、budget factor、candidate budget 和 super probes。
- [x] 4.2 为双层 routing 运行导出平均 child candidates scored 和 exact fallback count。
- [x] 4.3 增加可选的 exact-overlap 诊断，用于开发阶段比较双层 routing 选出的 clusters 与 exact top-`nprobe` clusters 的重合情况。
- [x] 4.4 确保这次 change 的 benchmark 结论必须基于真实 GT recall（`--skip-gt 0`），不能把 skip-GT 运行作为最终证据。

## 5. 测试

- [x] 5.1 为默认参数推导增加单元测试，覆盖 `4096`、`8192`、`16384` 等 `nlist` 值以及小规模边界情况。
- [x] 5.2 增加 hierarchy build 的单元测试，验证 child coverage 完整，且不存在缺失或重复的一层 centroid ids。
- [x] 5.3 增加查询测试，验证关闭双层 routing 时返回结果与 exact baseline 一致。
- [x] 5.4 增加查询测试，验证 hierarchy 不可用或 candidate 数不足时，双层 fallback 会安全触发。
- [x] 5.5 增加 benchmark smoke coverage，确认新的 CLI 参数可以正确解析，且 JSON schema 保持向后兼容。

## 6. 性能验证

- [x] 6.1 使用现有 MSMARCO `fht_kac_rotator` baseline，在 exact coarse routing 模式下配合真实 GT recall 和 `--early-stop 0` 运行一次基线。
- [x] 6.2 在相同 benchmark 指令下，以默认 `m = ceil(nlist / 128)` 和 `budget = 8 * nprobe` 运行一次双层 routing diagnostic，并启用真实 GT recall、`--early-stop 0` 和 `--two-level-coarse-exact-overlap 1`。
- [x] 6.3 对比 `avg_query_time_ms`、`avg_coarse_select_ms`、`avg_coarse_score_ms`、`avg_coarse_topn_ms`、`recall@10`、child candidates scored 和 fallback count。
- [ ] 6.4 如果 diagnostic recall/overlap 可接受，再串行运行关闭 `--two-level-coarse-exact-overlap` 的 two-level timing，并对最佳且有效的双层配置运行 perf，验证 coarse scoring 采样占比下降，并且主要热点没有转移到 child scoring 或 top-n selection。
- [x] 6.5 如果 recall 出现明显下降，则继续 sweep `8`、`12`、`16` 三个 budget factor；在验证结果支持修改前，文档默认值仍保持为 `8`。

## 7. SuperKMeans 二层构建修正

- [x] 7.1 将 `IvfIndex::EnsureCoarseHierarchy()` 中的随机投影排序分桶替换为 `skmeans::SuperKMeans` 训练和 assignment。
- [x] 7.2 对 cosine/IP 索引使用 `normalized_centroids_` 训练二层，并设置 `angular=true`；非 cosine 路径使用原始 centroids。
- [x] 7.3 用 SuperKMeans assignment 构建 `child_offsets` 和 `child_centroid_ids`，确保所有一层 centroid 被覆盖且无重复。
- [x] 7.4 重新构建并运行 `test_ivf_index`，确认现有 two-level 单测仍通过。
- [x] 7.5 使用真实 GT、`--early-stop 0`、线性执行 exact baseline、two-level diagnostic 和必要时的 two-level timing。
