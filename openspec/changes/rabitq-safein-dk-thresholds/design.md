## Context

当前 ConANN 的阈值模型把同一个构建期 `d_k` 用在多个角色上：SafeIn 确认、旧版 SafeOut 分类，以及动态 SafeOut 的回退下界。现有诊断显示，基于 exact-L2 校准得到的 `d_k` 往往过于靠左，和 SafeIn 实际使用的 multi-bit RabitQ Stage2 距离分布并不对齐；而 SafeOut 已经有更强的 query-time 信号，即 estimate heap 的 kth frontier。

这次变更把这些职责拆开：
- SafeIn 使用一个专门在 RabitQ Stage2 距离空间中校准的阈值。
- SafeOut 仅在 estimate heap 充满后使用 query-time estimated kth distance 加 margin。
- 旧版 exact-L2 `d_k` 继续保留，用作兼容数据和老索引的回退。

## Goals / Non-Goals

**Goals:**
- 引入一个专用于 SafeIn 的 `d_k`，并从 multi-bit RabitQ Stage2 估计距离中校准出来。
- 避免 SafeIn 校准反过来削弱 SafeOut 剪枝。
- 当没有 SafeIn 专用阈值时，继续兼容旧索引并回退到现有 exact-L2 `d_k`。
- 在 metadata 和 benchmark 输出中暴露校准来源。

**Non-Goals:**
- 不改变 exact rerank 或最终 top-k 排序语义。
- 不在这次变更里引入 SafeIn / SafeOut 分离 epsilon。
- 不在没有 benchmark 证据和显式配置 / metadata 路径前，把 RabitQ-space `d_k` 设为默认。
- 不替换 CRC early-stop 校准。

## Decisions

1. SafeIn 阈值改为 `safein_d_k - 2 * margin`。

   原因：SafeIn 是唯一需要构建期接受阈值的分类分支。使用 RabitQ Stage2 校准值可以让阈值和 Stage2 SafeIn replay 实际使用的距离估计保持一致。

   备选方案：覆盖现有 `d_k_`。拒绝的原因是 `d_k_` 目前还承担 legacy exact-L2 校准数据，以及当前代码里 SafeOut 的回退下界。

2. SafeOut 阈值在 estimate heap 充满后改为 `dynamic_est_kth + 2 * margin`。

   原因：SafeOut 应该反映当前 query frontier。静态 `d_k` 下界会降低剪枝效果，并把 SafeOut 行为和 SafeIn 校准绑在一起。

   备选方案：继续保留 `max(est_heap_kth, static_d_k)`。在新模式下不采用，因为更大的 RabitQ SafeIn `d_k` 会直接削弱 SafeOut。

3. RabitQ SafeIn `d_k` 校准放在 RabitQ code / rotation 状态可用之后执行。

   原因：Stage2 距离估计需要 centroids、rotation、encoded codes、query preparation 和 bits。这个校准点更适合放在现有的 post-encoding calibration 路径附近，而不是 exact-L2 的 `CalibrateDk` 辅助函数里。

   校准流程：
   - 对每个 calibration query，选择配置好的候选域：full search 或 nprobe-limited serving 域。
   - 用 `EstimateDistanceMultiBit` 计算 `est_dist_s2`。
   - 取每个 query 的 Stage2 估计值里的 kth smallest。
   - 用现有的低分位规则聚合这些 query-level kth 值。

4. metadata 同时记录值和来源。

   需要记录的字段：
   - `safein_d_k`
   - `safein_dk_space = rabitq_s2`
   - `safein_dk_percentile`
   - `safein_dk_calibration_queries`
   - `safein_dk_search_scope`
   - `safein_dk_nprobe`（nprobe-limited 时）
   - `safein_dk_bits`

5. 运行时加载必须兼容旧索引。

   如果索引没有 SafeIn 专用阈值，运行时 MUST 使用 legacy exact-L2 `d_k` 作为 SafeIn 阈值。旧索引加载路径不能失败。

## Risks / Trade-offs

- RabitQ-space `d_k` 可能增加 false SafeIn。缓解方式：把它放在显式校准 / 配置开关后面，暴露 false SafeIn 诊断，并先在 COCO / MSMARCO 上验证。
- nprobe-limited 校准依赖 query 参数。缓解方式：在 metadata 中记录 `safein_dk_search_scope` 和 `safein_dk_nprobe`；把 runtime nprobe 不匹配当作可观察的诊断条件。
- full-search 校准在大数据集上可能很贵。缓解方式：同时支持 full 和 nprobe 两种模式；COCO 上用 full，MSMARCO 上用 nprobe-limited 分析。
- 移除 SafeOut 的 static `d_k` 回退后，在 estimate heap 还没满时可能降低剪枝。缓解方式：在 `est_heap.size() >= top_k` 之前保持 SafeOut 保守，并依赖现有 Uncertain / rerank 路径。

## Migration Plan

1. 增加 metadata 字段和运行时访问接口，并保留 legacy fallback。
2. 增加 SafeIn RabitQ Stage2 `d_k` 的 build / offline 校准。
3. 更新 `ClassifyAdaptive` 和调用方，分别使用 SafeIn 阈值和动态 SafeOut 阈值。
4. 扩展 diagnostics / benchmarks，打印 SafeIn `d_k` 的来源和 SafeOut 模式。
5. 在切换任何默认值之前，先在 COCO100k 和 MSMARCO 上验证。

回滚策略：关闭 RabitQ SafeIn `d_k` 模式，或加载没有 SafeIn 专用 metadata 的索引，此时自动回退到 legacy exact-L2 `d_k`。
