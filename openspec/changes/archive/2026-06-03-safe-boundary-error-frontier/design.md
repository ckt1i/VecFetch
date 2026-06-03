## Context

当前 dynamic SafeOut 路径中的 `est_heap_` 只维护 RaBitQ 估计距离上的 top-k heap。`ClusterProber` 使用当前 kth estimate 和每个候选自己的 margin 做分类，等价公式是：SafeOut 用 `d_hat > dynamic_d_k + 2e_i`，SafeIn 用 `d_hat < safein_d_k - 2e_i`。

这个模型把两类不同的不确定性混在了一起：一类是当前候选自己的误差界，另一类是定义 query-time top-k frontier 的那些候选各自的误差界。误差界依赖 query residual norm 和 candidate residual norm，因此它并不是跨候选恒定的。

目标模型应当是区间式的：

```text
候选下界: L_i = d_hat_i - e_i
候选上界: U_i = d_hat_i + e_i
```

SafeOut 应当用 `L_i` 去比较 query-time top-k 上界 frontier；SafeIn 应当用 `U_i` 去比较一个保守的、基于原始向量精确距离校准的接受阈值。

## Goals / Non-Goals

**Goals:**

- 在每个候选误差界不同的情况下，让 SafeOut 保持保守。
- 不维护第二个精确 upper-bound heap，而是用一个放宽后的上界 frontier，把 SafeOut 的 CPU 开销控制住。
- 让 SafeIn 使用一个显式的 full-vector exact-distance 接受阈值，并采用候选上界语义。
- 保留现有 FastScan estimate kernel 作为独立优化边界；本次只改分类阈值。
- 增加 COCO100k 仅向量搜索验证，报告 `nlist=2048`、`nprobe=64` 下两阶段 SafeIn / SafeOut / Uncertain 的分布。

**Non-Goals:**

- 不重做 RaBitQ 距离估计或 FastScan packed-code / LUT kernel。
- 不改变 exact rerank 或最终 top-k 排序语义。
- 不要求在热路径里再维护一个 `d_hat + e` 的第二个 heap。
- 不主动移除 legacy classification API，除非显式处理了兼容性。

## Decisions

1. 将 estimate heap 条目表示为 `{est_dist, error_bound}`。

   `est_dist` 仍然作为 heap 排序 key，这样可以尽量保留现有 kth-estimate 行为。`error_bound` 表示与该估计距离对应的候选 SafeOut 误差半径。heap 的 max-heap comparator 继续按 `est_dist` 排序。

   备选方案：单独维护一个按 `est_dist + error_bound` 排序的 heap。它对 `kth(upper)` 更精确，但会带来更多 heap 维护，以及更高的热路径分支和 cache 压力。

2. 维护一个 query-time 导出标量 `max_error_in_est_heap`。

   放宽后的 top-k 上界 frontier 定义为：

   ```text
   safeout_frontier_upper = kth_est_dist + max_error_in_est_heap
   ```

   这是保守的，因为当前 `est_dist` top-k heap 中的每个条目都满足 `est_dist <= kth_est_dist`，同时每个条目的 `error_bound <= max_error_in_est_heap`。因此这些条目的上界都不会超过 `kth_est_dist + max_error_in_est_heap`。

   为了简单和稳妥，在 heap replacement 之后用一次 O(K) 扫描重算 `max_error_in_est_heap` 是可以接受的，因为 K 很小。如果 profiling 显示这部分有明显开销，后续再用 dirty flag 或辅助 max 结构优化。

3. 将 SafeOut 分类改成候选下界语义。

   SafeOut SHALL 使用：

   ```text
   d_hat_i - e_i > safeout_frontier_upper
   ```

   对应的 SIMD mask 阈值形式是：

   ```text
   d_hat_i > safeout_frontier_upper + e_i
   ```

   Stage1 FastScan 可以通过传入 `safeout_frontier_upper` 作为 base threshold 来实现，并使用 `+ margin_factor * norm_oc`，而不是 `+ 2 * margin_factor * norm_oc`。

4. 将 SafeIn 分类改成候选上界语义。

   SafeIn SHALL 使用：

   ```text
   d_hat_i + e_i < d_k_fullvector_safein
   ```

   对应的阈值形式是：

   ```text
   d_hat_i < d_k_fullvector_safein - e_i
   ```

   这个阈值应当是一个基于原始向量精确距离校准得到的 SafeIn acceptance radius，而不是 dynamic SafeOut frontier。现有 metadata 和 builder 路径可以在配置允许时继续复用 legacy exact-distance d_k 校准路径，但运行时公式必须采用候选上界语义。

5. 保持 heap 未满时的 SafeOut 保守行为。

   当 estimate heap 中少于 `top_k` 个条目时，SafeOut frontier 为 `+inf`；此时不应触发任何 estimate-driven SafeOut。这样可以保留当前代码本来就想要的保守行为。

6. 保留 FastScan estimate-kernel 的优化边界。

   `EstimateDistanceFastScan`、packed sign layout、LUT prepare 和 single-bit accumulation 继续与 post-estimate classification 分离。本次只更新 Stage1 mask 阈值和向后传递的 candidate metadata，不应阻塞 ongoing 的 single-bit FastScan kernel 优化。

## Risks / Trade-offs

- 放宽后的 frontier 可能比旧公式或精确 `kth(d_hat + e)` 产生更少的 SafeOut。
  缓解：先在 COCO100k 上验证 SafeOut / SafeIn / Uncertain 数量和 recall 影响，再解释性能结果。

- heap 更新后重算 `max_error_in_est_heap` 会带来 O(K) 开销。
  缓解：K 在预期使用场景中很小；先用现有 timing 字段测量，只有 profiling 证明回归时再优化。

- 若复用过大的 full-vector d_k 作为 SafeIn 阈值，可能增加 false SafeIn。
  缓解：把这个阈值视为 acceptance radius，并在设为默认值前验证 false SafeIn / recall 指标。

- 诊断和 replay 工具可能静默地继续使用旧的 `2 * margin` 公式。
  缓解：更新 replay/diagnostic 输出，或明确标记旧列，使验证时使用新的区间语义。

- Stage2 SIMD 分类当前假设的是共享 double-margin 公式。
  缓解：同步更新 scalar 和 SIMD 路径，并为 split-margin 与 non-split-margin 路径增加等价性测试。

## Migration Plan

1. 为 estimate heap 条目增加候选误差界元数据，并在每次 query 开始时重置导出的 frontier 状态。
2. 将 Stage1 / Stage2 分类得到的候选误差界传入 `CandidateBatch` 和 CRC estimate buffer。
3. 在 `OverlapScheduler` 中更新 SafeOut frontier 计算，并把上界阈值传给 `ClusterProber`。
4. 更新 Stage1 FastScan、Stage2 scalar、Stage2 SIMD 以及 `ConANN::ClassifyAdaptive` 的阈值公式。
5. 更新暴露 SafeIn / SafeOut 公式的 diagnostics / replay 工具。
6. 为区间式 SafeIn / SafeOut 边界和 heap-not-full 行为增加单元测试。
7. 构建并运行 COCO100k 的仅向量 `bench_vector_search.cpp` 验证，配置为 `nlist=2048`、`nprobe=64`。

回滚方案：如果验证发现回归且无法用更安全的 frontier 语义解释，则恢复旧的 `dynamic_d_k + 2 * margin` 公式和 heap 条目类型。

## Open Questions

- 生产默认配置下，`d_k_fullvector_safein` 应采用哪个 exact percentile 和 sampling policy 作为 acceptance radius？
- 过渡阶段的 benchmark 输出，是否需要同时包含旧 SafeOut 阈值和新 SafeOut 阈值，便于 replay 对照？
