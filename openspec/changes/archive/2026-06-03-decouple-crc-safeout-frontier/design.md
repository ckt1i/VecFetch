## Context

现有 `OverlapScheduler` 中 `use_crc_ = config.crc_params != nullptr` 同时承担了两个职责：

- 启用 CRC early-stop，并在 probe loop 中调用 `CrcStopper::ShouldStop()`。
- 启用 estimate heap，并从该 heap 导出 `safeout_frontier_upper` 供 Stage1/Stage2 SafeOut 剪枝使用。

这导致关闭 CRC 或关闭 early-stop 时，dynamic SafeOut 的行为不可独立控制。当前 SafeOut frontier 也不是严格的 `kth(d_hat + e)`，而是先按 `d_hat` 保留 top-k，再取这些候选的最大上界。由于 `e_i` 是 query-candidate 级误差界，frontier 应直接在 upper-bound 空间中维护。

## Goals / Non-Goals

**Goals:**

- 将 CRC early-stop 和 dynamic SafeOut frontier 拆成两个独立功能。
- 支持 `early_stop=false` 或 CRC disabled 时仍启用 dynamic SafeOut。
- 将 SafeOut frontier 改为 `F = kth_smallest(d_hat + e)`。
- 保持 SafeOut 判定为候选下界与 frontier 上界的 interval 比较：`d_hat_i - e_i > F`。
- 保持 cluster 级 frontier 快照，避免当前 cluster 内候选相互影响。
- 保留现有 RaBitQ/FastScan estimate kernel 的优化边界。

**Non-Goals:**

- 不修改 final exact rerank 的语义。
- 不重做 RaBitQ/FastScan 距离估计公式。
- 不要求 SafeOut frontier 使用 exact full-vector top-k；exact frontier 可作为后续优化。
- 不把 candidate-level SafeIn CRC 校准合并进本 change。

## Decisions

1. 引入独立的 dynamic SafeOut 开关和状态。

   `SearchConfig` 需要区分：

   ```text
   enable_dynamic_safeout
   crc_params / enable_crc_early_stop
   early_stop
   ```

   `enable_dynamic_safeout` 控制是否维护 SafeOut frontier 以及是否向 `ClusterProber` 传入有限 frontier。`crc_params` 只表示 CRC 参数是否存在。`early_stop` 只控制 probe loop 是否允许提前 break。

   备选方案是继续复用 `use_crc_`，但在 `early_stop=false` 时仍维护 heap。这个方案仍然把 SafeOut 依赖到 CRC 参数加载上，不能解决 `--crc 0` 下无法评估 SafeOut 的问题。

2. 使用 top-k upper-bound heap 构造 SafeOut frontier。

   对每个进入 frontier state 的候选记录：

   ```text
   d_hat = candidate estimated distance
   e     = candidate error radius
   U     = d_hat + e
   ```

   维护一个 size 至多为 `top_k` 的 max-heap，排序 key 为 `U`。heap 中保存当前已见候选里 `U` 最小的 top-k 个候选。当 heap 满时：

   ```text
   F = heap.top().U = kth_smallest(U)
   ```

   新候选更新规则：

   ```text
   if heap.size < top_k:
       push(candidate)
   else if U < heap.top().U:
       pop heap.top()
       push(candidate)
   else:
       ignore
   ```

   这比旧的 `topk_by_d_hat -> max(d_hat+e)` 更直接，也避免 frontier 选点和 frontier 值使用不同排序标准。

3. CRC early-stop 保留独立 kth score state。

   CRC calibration 当前的 `CrcStopper::ShouldStop(probed_count, current_kth_dist)` 语义依赖校准时使用的 score。为了避免改变 CRC 风险目标，CRC early-stop 不应直接复用 SafeOut 的 upper-bound heap。

   当 CRC early-stop 启用时，运行时可以继续维护一个按 CRC score 排序的 top-k state，例如当前按 `d_hat` 的 estimate heap。这个 state 只为 `current_kth_dist` 服务；SafeOut state 只为 `F=kth(U)` 服务。二者可消费同一批 candidate estimates，但启用条件和导出值必须独立。

4. SafeOut 判定使用 interval lower-bound 语义。

   对待分类 candidate：

   ```text
   L_i = d_hat_i - e_i
   SafeOut_i := L_i > F
   ```

   在现有 SIMD/标量阈值形式中等价为：

   ```text
   d_hat_i > F + e_i
   ```

   Stage1 使用：

   ```text
   e_i_s1 = 2 * ||q-c|| * ||o_i-c|| * safeout_eps
   ```

   Stage2 使用：

   ```text
   e_i_s2 = e_i_s1 / 2^(bits - 1)
   ```

   SafeOut frontier heap 应使用 candidate 最终进入 sink 时对应 stage 的 `d_hat` 和 `e`。Stage2 SafeOut 的 candidate 不进入 frontier；Stage2 SafeIn/Uncertain 使用 Stage2 的 `d_hat/e` 进入 frontier。

5. 保持 cluster 级 frontier 快照。

   进入 `ProbeCluster` 时读取一次：

   ```text
   F = dynamic_safeout_state.frontier_or_inf()
   ```

   当前 cluster 内所有 Stage1/Stage2 分类使用同一个 `F`。当前 cluster 的 surviving candidates 在 cluster 结束时合并到 frontier state，只影响后续 cluster。

6. CLI 和统计字段需要反映解耦。

   Benchmark 应能表达至少四类组合：

   ```text
   dynamic_safeout=0, crc_early_stop=0
   dynamic_safeout=1, crc_early_stop=0
   dynamic_safeout=0, crc_early_stop=1
   dynamic_safeout=1, crc_early_stop=1
   ```

   JSON/日志中需要区分：

   ```text
   crc_enabled
   early_stop_enabled
   dynamic_safeout_enabled
   crc_would_stop / early_stopped
   safeout_frontier_updates
   ```

## Risks / Trade-offs

- [Risk] 维护 upper-bound heap 会增加每个 surviving candidate 的 O(log top_k) 成本。  
  Mitigation: `top_k` 通常很小，先实现清晰版本，并用现有 timing 字段验证；若有回归再做批量更新或小 K 固定数组优化。

- [Risk] 同时启用 CRC 和 dynamic SafeOut 时维护两个 top-k state 会增加 CPU 压力。  
  Mitigation: 两个 state 语义不同，先保证正确性；后续可在确认 CRC score 与 SafeOut upper score 可兼容后再考虑合并。

- [Risk] `F=kth(U)` 可能比旧 frontier 更保守，SafeOut 数量下降。  
  Mitigation: 在 COCO100k `nlist=2048,nprobe=64` 上报告 Stage1/Stage2 SafeOut/Uncertain 变化，并和 recall/latency 一起解释。

- [Risk] benchmark 参数默认值变化可能影响历史结果可比性。  
  Mitigation: 输出中显式记录 `dynamic_safeout_enabled`，并保留关闭开关用于复现实验。

## Migration Plan

1. 在 `SearchConfig` 中加入 dynamic SafeOut 独立配置，并在 benchmark CLI 中暴露对应参数。
2. 在 `OverlapScheduler` 中拆分 CRC early-stop state 和 dynamic SafeOut frontier state。
3. 实现 `F=kth(d_hat+e)` 的 SafeOut frontier heap，并在每个 query 开始时重置。
4. 将 cluster 入口处的 frontier 快照传入 `ClusterProber`。
5. 保持 Stage1/Stage2 SafeOut 判定公式为 `d_hat > F + e_i`，确认 SIMD 与标量路径一致。
6. 更新 stats、JSON、日志字段，区分 CRC early-stop 与 dynamic SafeOut。
7. 增加单元测试和 COCO100k vector-only 验证。

回滚策略：保留 dynamic SafeOut 开关；如新 frontier 在验证中出现不可接受回归，可通过配置关闭 dynamic SafeOut 或临时回退到旧 frontier state 进行对照。

## Open Questions

- `enable_dynamic_safeout` 在 benchmark 中是否默认开启？建议默认开启，并通过显式 `--dynamic-safeout 0` 关闭以复现 no-SafeOut baseline。
- CRC early-stop 的独立 kth score state 是否继续使用 `d_hat`，还是应在后续 change 中重新校准为 upper-bound score？本 change 建议不改变 CRC 校准目标。
