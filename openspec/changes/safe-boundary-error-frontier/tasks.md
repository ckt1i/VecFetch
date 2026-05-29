## 1. 运行时 Frontier 状态

- [x] 1.1 将 estimate heap 条目类型替换为同时保存 `d_hat` 和候选 `error_bound` 的结构，并保持 heap 继续按 `d_hat` 排序。
- [x] 1.2 增加 per-query 的 `max_error_in_est_heap` 状态，并在每次 `Search()` 开始时重置。
- [x] 1.3 更新 CRC / estimate buffering，使向后传递的候选同时携带 `est_dist` 和用于 heap 维护的 SafeOut 误差界。
- [x] 1.4 更新 heap 插入和替换逻辑，维护 `max_error_in_est_heap`；除非 profiling 证明不够，否则替换后使用一次 O(K) 重算。
- [x] 1.5 仅当 estimate heap 已满时计算 `safeout_frontier_upper = est_heap_.front().d_hat + max_error_in_est_heap`；否则使用 positive infinity。

## 2. 分类公式修改

- [x] 2.1 扩展 `CandidateBatch` 或等价的 probe-to-sink metadata，使其能携带 per-candidate error bound。
- [x] 2.2 更新 `ConANN::ClassifyAdaptive` 或查询路径中的等价逻辑，使 SafeOut 使用 `approx_dist - safeout_margin > safeout_frontier_upper`。
- [x] 2.3 更新 SafeIn 分类逻辑，使其使用 `approx_dist + safein_margin < fullvector_safein_d_k`。
- [x] 2.4 将 Stage1 FastScan 的 SafeOut mask 阈值从 `base + 2 * margin` 改为 `safeout_frontier_upper + margin`。
- [x] 2.5 将 Stage1 FastScan 的 SafeIn mask 阈值从 `base - 2 * margin` 改为 `fullvector_safein_d_k - margin`。
- [x] 2.6 更新 Stage2 scalar 分类逻辑，使 SafeIn 和 SafeOut 使用单边候选 margin。
- [x] 2.7 更新 Stage2 SIMD 分类逻辑，使其与 scalar 的单边 margin 公式一致。
- [x] 2.8 保持 heap-not-full 行为保守，在 `top_k` 个 estimate 尚未保留前不允许 estimate-driven SafeOut 触发。

## 3. 兼容性与诊断

- [x] 3.1 在可行范围内保持现有 `Classify(approx_dist)`、`Classify(approx_dist, margin)` 和 `ClassifyAdaptive` overload 的源码兼容。
- [x] 3.2 确保没有 dedicated full-vector SafeIn acceptance threshold 的旧索引会回退到已有的 legacy exact-distance `d_k`。
- [x] 3.3 更新当前仍编码 `dynamic_d_k + 2 * margin` 的 diagnostic 和 replay 工具，使其报告新的边界模式或计算新的阈值。
- [x] 3.4 保持 FastScan estimate-kernel 的 timing 和优化边界可见，不要把 estimate generation 和 post-estimate classification mask 合并。

## 4. 测试

- [x] 4.1 为不同候选误差界和 frontier 误差界组合增加 SafeOut 下界分类单元测试。
- [x] 4.2 为使用 full-vector SafeIn acceptance threshold 的 SafeIn 上界分类增加单元测试。
- [x] 4.3 增加 heap-not-full 行为测试，确认在 estimate heap 未满前 SafeOut 一直被禁用。
- [x] 4.4 增加 heap replacement 和 `max_error_in_est_heap` 重算测试。
- [x] 4.5 为 Stage2 单边 margin 分类增加 scalar 与 SIMD 等价性覆盖。

## 5. COCO100k 仅向量验证

- [x] 5.1 在启用 benchmarks 的配置下构建 `bench_vector_search`。
- [x] 5.2 在 COCO100k 上以仅向量模式重跑 `bench_vector_search.cpp`，配置为 `nlist=2048`、`nprobe=64`。
- [x] 5.3 记录新边界逻辑下的 Stage1 SafeIn、SafeOut 和 Uncertain 数量。
- [x] 5.4 在启用 Stage2 时，记录新边界逻辑下的 Stage2 SafeIn、SafeOut 和 Uncertain 数量。
- [x] 5.5 将最终 recall / false SafeOut / false SafeIn 指标与可用 baseline 或上一轮结果进行比较。
- [x] 5.6 将验证命令、数据集路径或别名、构建配置和 summary 输出归档到 change notes 或 benchmark 输出目录。
