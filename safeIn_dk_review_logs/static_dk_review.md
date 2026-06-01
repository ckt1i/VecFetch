# 全局静态 SafeIn d_k 的 Review 结论

## 背景

当前 SafeIn 的线上判定可抽象为：

```text
U_i = d_hat_i + e_i
SafeIn(i) iff U_i < T
```

其中 `T = safein_d_k` 是从离线 query-to-base exact top-k 半径样本中按 percentile 取出的全局静态阈值。实际目标则是 candidate-level prefetch purity，即希望 SafeIn 中真正不属于 query top-k 的候选尽可能少。

最近 fixed-nprobe sweep 观察到：

| safein_dk_percentile | safein_d_k | total SafeIn | false SafeIn | false / total |
|---:|---:|---:|---:|---:|
| 0.90 | 1.30685 | 2483 | 1010 | 40.68% |
| 0.95 | 1.29626 | 1428 | 483 | 33.82% |
| 0.97 | 1.28587 | 843 | 259 | 30.72% |

旧版 `safe-boundary-error-frontier` 在带 CRC early-stop 时 p=0.90 的 false / total 约 24.72%，但 recall@10 只有 0.8986；当前 fixed-nprobe recall@10 约 0.9558。因此旧版比例更低不能直接说明阈值更好，它也混入了 early-stop 对 candidate domain 的过滤。

## 核心判断

用户提出的猜想是合理的：全局静态 `T` 的问题不只是“整体过宽松”，而是它没有条件化到每个 query 的真实 top-k 半径 `R_q`。

如果误差界成立且 GT/ID 口径一致，则：

```text
U_i < T <= R_q  =>  d_i <= U_i < R_q
```

这意味着候选应当属于 query 的 exact top-k 范围。反过来，false SafeIn 的主要结构性来源是：

```text
T > R_q
```

此时 easy query 的真实 top-k 半径较小，全局 `T` 过松，会把 `(R_q, T)` 区间里的非 top-k 近邻也判成 SafeIn。

## 与“d_k 过宽松”的区别

`d_k` 过宽松是全局 scalar 问题：把 `T` 整体调小可以减少 false，但会牺牲所有 query 的 SafeIn 覆盖。

`R_q` 异质性是条件分布问题：同一个 `T` 对 easy query 过松，对 hard query 过紧。因此继续只调全局 percentile 很难同时得到高 coverage 和高 purity。

用当前 exact `R_q` 样本观察，`R_q` 分布有明显跨度：

| quantile | R_q |
|---:|---:|
| min | 1.2213 |
| 1% | 1.2652 |
| 5% | 1.2962 |
| 10% | 1.3068 |
| 50% | 1.3557 |
| 90% | 1.4046 |
| max | 1.5007 |

p=0.90 的 `T≈1.30685` 对应低 10% 半径附近。它对大多数 query 是保守的，但对低半径 easy query 会过松；这些 query 可能贡献大量 false SafeIn，因此 candidate-level false ratio 可以远大于 query-level 低尾比例。

## 最小验证分解

本轮验证按三个问题展开：

1. 按 query 统计 `R_q`、`T-R_q`、`SafeIn_q`、`falseSafeIn_q`，检查 false 是否集中在 `T > R_q` 的 query。
2. 对 p=0.90 的候选级 false SafeIn 分类：
   - `static_T_above_query_Rq`：`R_q < d_i < T`，说明 false 来自静态阈值高于该 query 半径。
   - `bound_violation_exact_gt_T`：`U_i < T < d_i`，说明估计上界没有覆盖 exact distance。
   - `gt_tie_or_id_mismatch`：`d_i <= R_q` 但不在 GT top-k，通常是 tie 或 ID/GT 口径问题。
3. 用 oracle per-query 阈值 `T_q=R_q` replay 一遍，估计如果阈值能按 query 自适应，SafeIn/false 的上限改善空间。

## 方法方向

如果验证成立，后续不应继续只调全局 `safein_dk_percentile`。更合理的方向是 query-adaptive SafeIn threshold：

- 使用 coarse score gap、query-to-centroid 距离、probe 内 `kth(U)` 或 current frontier 估计每个 query 的 `R_q` 下界。
- 全局 `T` 只作为 fallback 或 cap，而不是主判定。
- 若仍需统计保证，应把目标改成 candidate-level 或 online replay aligned 的 purity/FDP 目标，而不是 query-level `R_q` percentile。

