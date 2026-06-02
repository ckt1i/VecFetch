# 动态 SafeIn d_k / Payload Prefetch 方案设计

## 静态 Review 的证据

现有验证表明，全局静态阈值 `T=safein_d_k` 的主要问题出现在 easy query 上：这些 query 的真实 top-k 半径 `R_q` 小于全局阈值，也就是 `T > R_q`。

- p=0.90：99.50% 的 false SafeIn 来自 `T > R_q` 的 query；`T<=R_q` query 上的 false rate 只有 0.85%。
- p=0.95：99.59% 的 false SafeIn 来自 `T > R_q` 的 query；`T<=R_q` query 上的 false rate 只有 0.40%。
- p=0.97：99.61% 的 false SafeIn 来自 `T > R_q` 的 query；`T<=R_q` query 上的 false rate 只有 0.29%。

因此，本轮实验目标不是继续只调一个静态 percentile，而是在查询期间构造 query-level 动态阈值：对 easy query 降低浪费的 SafeIn payload prefetch，同时尽量保持 recall 不变。

## 已实现的候选方案

所有模式都可以通过以下参数控制：

```text
--dynamic-safein static|frontier_cap|frontier_delay|frontier_stable|frontier_scale
--dynamic-safein-payload-only 0|1
--dynamic-safein-min-probes N
--dynamic-safein-stable-probes N
--dynamic-safein-rel-tol X
--dynamic-safein-scale X
--dynamic-safein-scale-cap-static 0|1
```

### 1. 静态 Baseline

保持现有行为不变，对比 p=0.90、p=0.95、p=0.97。它们用于复现静态阈值的 tradeoff 曲线：

- p=0.90：速度最快，但 false SafeIn 较多。
- p=0.95：更保守，false SafeIn 明显下降，延迟代价较小。
- p=0.97：更强 purity，但延迟代价更大。

### 2. Upper Frontier Cap

使用查询期间维护的 kth upper-bound frontier：

```text
U_i = d_hat_i + e_i
F_upper = kth_smallest(U_i)
T_q = min(T_static, F_upper)
```

这个方案成本低，适合作为 sanity check。但预期效果不会特别强，因为 `F_upper` 本质上更接近 SafeOut frontier，而不是 SafeIn purity 的保证。它可以避免部分过大的静态阈值，但对 false SafeIn 的约束通常不够紧。

### 3. Lower Frontier Delay / Stable

维护 kth lower-bound frontier：

```text
L_i = d_hat_i - e_i
F_lower = kth_smallest(L_i)
T_q = min(T_static, F_lower)
```

`F_lower` 对 SafeIn 更保守。直觉上，当候选空间足够覆盖真实近邻时，至少 k 个真实 top-k 候选的 lower bound 不应大于该 query 的 top-k 半径。因此，用 `F_lower` 限制 SafeIn 阈值，可以更有效地减少 easy query 上的 false SafeIn。

本轮实现了两个启用规则：

- `frontier_delay`：只要 frontier 已经形成，就开始使用动态阈值。
- `frontier_stable`：等待连续若干次 frontier 更新变化低于阈值后，再启用动态阈值。

`frontier_delay` 更早生效，延迟代价通常更低；`frontier_stable` 更保守，purity 更强，但可能推迟 SafeIn 启用，增加最终 payload fetch 和整体延迟。

### 4. Calibrated Upper Frontier Scale

使用缩放后的 upper frontier：

```text
T_q = scale * F_upper
```

这个方案希望用 `scale` 校准 `F_upper`，让它近似 query-level top-k 半径，同时避免 lower-bound frontier 过于严格。第一轮 sweep 测试了 `scale=0.92/0.94`。

它的风险是：`F_upper` 仍然是偏松的 upper-bound proxy。即使缩放后能压低 payload prefetch，也可能只是把 I/O 从 SafeIn prefetch 延后到最终 payload fetch，并不一定改善端到端延迟。

### 5. Payload-Only Gate

启用：

```text
--dynamic-safein-payload-only 1
```

此时静态 SafeIn 分类器保持不变，动态阈值只控制是否发起 `VEC_ALL` payload prefetch。只有当候选的 SafeIn upper bound 被动态阈值接受时，才读取 vector+payload；否则先只读取 vector，最终进入 top-k 的 payload 在最后补取。

这是 recall 风险最低的方案，因为它不改变候选分类，只改变 payload 预取策略。它直接针对浪费的 SafeIn payload prefetch，但如果 payload 被大量延后，可能增加 final fetch，从而拉高端到端延迟。

### 6. Classification-Changing 动态阈值

对应：

```text
--dynamic-safein-payload-only 0
```

此时动态阈值不仅控制 payload prefetch，也会进入 SafeIn 分类逻辑。候选需要满足动态阈值下的 SafeIn 条件，才会被判为 SafeIn。

这个方案风险更高，因为它改变了分类边界；但它也是本轮最有效的方向。实验结果显示，仅 gate payload 不能降低 false SafeIn 统计，而把 lower frontier 接入分类阈值后，可以显著减少 false SafeIn 和无效 `all_read_requests`。

## 胜出标准

主要指标：

- recall@10 相比静态 p=0.90/p=0.95 不能有实质性退化。
- false SafeIn rate 和 false SafeIn count 要下降。
- `all_read_requests` / `safein_payload_prefetched` 要下降，同时不能显著增加 `remaining_payload_fetches`。
- avg/p95/p99 latency 要更低或至少接近 baseline。

辅助判断：

- 如果 classification-changing 方案没有明确改善 latency，则优先考虑 payload-only 方案，因为 recall 风险更低。
- 如果 delay 规则在 query 间波动较大，则优先考虑 stable 规则。
- 如果动态方案收益不稳定，则使用 `static_p095` 作为保守 baseline。

## 本轮实验后的设计判断

实验结果显示，最有效的方向是 lower-bound frontier + classification-changing 动态阈值。

- `lower_delay_classify_p090` 是当前综合最好的方案：大幅降低 false SafeIn 和 `all_read_requests`，recall 基本不变，延迟代价可控。
- `lower_stable_classify_p090` 是最高 purity 方案：false SafeIn 几乎被压到很低，但延迟代价明显更高。
- payload-only 方案虽然能减少 SafeIn payload prefetch，但 false SafeIn 分类不变，并且 final payload fetch 增加，端到端延迟没有收益。
- upper frontier 相关方案只能作为对照，不适合作为主方案。

因此，默认推荐从 `lower_delay_classify_p090` 开始；如果目标是证明 SafeIn purity，则选择 `lower_stable_classify_p090`；如果希望低风险上线对照，则选择 `static_p095`。
