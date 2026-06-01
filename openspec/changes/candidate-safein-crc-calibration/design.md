## Context

当前 SafeIn 运行时已经采用候选上界语义：

```text
U_i = d_hat_i + e_i
SafeIn_i := U_i < T
```

上一轮验证显示，false SafeIn 主要来自全局阈值 `T` 高于某些查询的真实 top-k 半径，而不是候选误差界 `e_i` 大量失效。因此继续从 query-level `r_k(q)` 或 RabitQ kth 值取分位数，不能直接控制最终关心的 candidate-level `falseSafeIn / SafeIn`。

本变更把 SafeIn 校准目标改成候选级 replay 目标：离线复现线上候选访问和分类路径，用 full-vector exact top-k 只作为 falseSafeIn 标签来源，然后在量化上界 `U_i` 空间选择阈值。

## Goals / Non-Goals

**Goals:**

- 新增 candidate-level SafeIn CRC 校准模式，直接选择满足 `falseSafeIn / SafeIn <= beta` 的全局 SafeIn 阈值。
- 线上比较保持简单形式：`U_i = d_hat_i + e_i`，`SafeIn_i := U_i < T`。
- calibration 集用于选阈值，held-out validation 集用于报告实际 candidate-level ratio 和 SafeIn 收益。
- 保留旧的 exact-L2 / RabitQ kth SafeIn `d_k` 校准路径，便于回退和对照。

**Non-Goals:**

- 不引入 query-local top-k lower-bound frontier。
- 不修改 RaBitQ / FastScan 距离估计 kernel。
- 不把原始向量距离用于线上 SafeIn 比较。
- 不承诺简单 CRC rank 公式直接适用于 `falseSafeIn / SafeIn` 比值；该比值用于候选级阈值搜索和 held-out 验证。

## Decisions

1. 用 candidate replay 选择阈值，而不是继续采样 query-level kth。

   对每个 calibration query，系统按线上候选域生成候选集合 `C_q`，为每个候选记录：

   ```text
   U_i = d_hat_i + e_i
   is_false_i = candidate_id not in exact_fullvector_topk(q)
   stage = stage1 或 stage2/final
   ```

   然后把候选按 `U_i` 升序排序，对前缀统计：

   ```text
   S_m = m
   F_m = prefix false count
   R_m = F_m / max(S_m, 1)
   ```

   选择满足 `R_m <= beta` 且 `S_m` 最大的前缀，并把对应边界写成 `T`。这样阈值直接优化 SafeIn 数量，同时约束候选级 false ratio。
   这里的 `beta` 是条件比例 `falseSafeIn / SafeIn`，不是 `falseSafeIn / all_candidates`。

2. exact full-vector top-k 只用于标签，不作为阈值样本。

   校准仍需要真实 top-k 集合 `G_q` 判断 falseSafeIn，否则无法定义 `falseSafeIn / SafeIn`。但阈值不再来自 `r_k(q)` 分位数，而来自 replay 得到的 `U_i` 分布。

3. calibration replay 的候选域必须匹配线上 serving 配置。

   如果线上使用 `nlist=2048, nprobe=64`，校准 replay 也应使用相同 `nprobe=64` 来生成候选流。full IVF 或大 nprobe 可以用于 ground truth/stress test，但不应替代线上候选分布，否则 candidate-level ratio 会出现分布偏移。

4. 运行时只消费一个全局阈值。

   在线路径不需要维护 per-query candidate 前缀统计。现有 Stage1 / Stage2 mask 可以继续使用等价形式：

   ```text
   d_hat_i < T - e_i
   ```

   这保持 FastScan 路径的比较结构稳定。

5. 新阈值来源需要显式 provenance。

   metadata 或 sidecar artifact 需要记录：

   ```text
   safein_threshold_source = candidate_crc
   beta
   threshold_T
   calibration_queries
   validation_queries
   candidate_domain
   nprobe
   bits
   calibration_false_safein_rate
   validation_false_safein_rate
   ```

## Risks / Trade-offs

- `falseSafeIn / SafeIn` 是候选级经验比值，不是原有 query-level CRC 分位公式。→ 使用 calibration/validation 分离，并在报告中明确区分 calibration ratio 与 held-out validation ratio。
- calibration set 上的阈值可能过拟合具体查询或候选分布。→ 默认要求 held-out validation，并记录 query split、seed 和样本数。
- 若 calibration replay 使用 full IVF，而线上使用小 nprobe，阈值可能不匹配线上分布。→ 校准 artifact 必须记录 candidate domain，默认使用线上 nprobe 域。
- 过低的 `beta` 会显著减少 SafeIn 数量。→ validation 输出同时报告 SafeIn 数、falseSafeIn/SafeIn、Stage1/Stage2 分布、Uncertain 数量和 query-block bootstrap 95% CI。
- 旧索引没有 candidate CRC metadata。→ 保留 legacy SafeIn `d_k` fallback，不要求重建旧索引。

## Migration Plan

1. 增加 candidate CRC 阈值来源的配置和 metadata 枚举，默认仍保持 legacy 路径。
2. 在离线/benchmark 校准路径中实现候选 replay 和阈值选择。
3. 在 `bench_vector_search` 或专用 replay 工具中加载 candidate CRC 阈值，并输出 calibration/validation 指标。
4. 验证 COCO100k `nlist=2048, nprobe=64` 下 candidate-level ratio 与 SafeIn 收益。
5. 只有当新模式验证稳定后，再考虑把它作为推荐 SafeIn 校准路径。

## Open Questions

- 默认 sweep 先使用 `beta=0.05,0.10,0.20`，后续需要由 COCO100k 和更多数据集验证是否需要更细网格。
- 阈值应优先按 final SafeIn 统计选择，还是分别支持 Stage1-only / Stage2-final 两种目标。
- 是否需要把 bootstrap 置信区间纳入正式 validation 输出。
