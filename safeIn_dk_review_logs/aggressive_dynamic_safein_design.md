# Frontier Dynamic SafeIn / Deferred Prefetch 方案设计

## 背景

Dynamic SafeIn 的探索阶段曾测试过 upper frontier、lower delay/stable、
payload-only gate、`frontier_blend` 和 gap gate。后续实验收敛到一个更窄的
实现：

```text
--dynamic-safein frontier
```

该模式等价于此前补跑中的 `blend000_defer4`：不再使用
`lambda * (F_upper - F_lower)` 插值，不再暴露 scale/gap/payload-only 调参。
query-adaptive SafeIn threshold 固定为当前 lower-bound top-k frontier。

## 核心记号

对查询 `q` 的每个候选 `i`，probe 阶段得到：

```text
d_hat_i = estimated distance
e_i     = estimate error bound
L_i     = lower bound
U_i     = upper bound
```

调度器在线维护两个 frontier：

```text
F_lower = kth_smallest(L_i)
F_upper = kth_smallest(U_i)
```

`frontier` Dynamic SafeIn 只使用 `F_lower`：

```text
T_q = F_lower
```

`F_upper` 仍由 Dynamic SafeOut 使用，不参与 SafeIn threshold 插值。

## 支持的模式

当前 runtime 和 benchmark 只支持：

```text
--dynamic-safein static
--dynamic-safein off
--dynamic-safein frontier
```

语义：

- `static` / `off`：使用 index 中的 global `safein_d_k`，不依赖 Dynamic
  SafeIn frontier。
- `frontier`：frontier ready 后使用 `T_q = F_lower` 作为 SafeIn
  classification 和 payload prefetch threshold。

旧模式和旧 flag 已删除：

```text
frontier_cap
frontier_delay
frontier_stable
frontier_scale
frontier_blend
--dynamic-safein-scale
--dynamic-safein-scale-cap-static
--dynamic-safein-payload-only
--dynamic-safein-gap-rel-tol
--dynamic-safein-gap-abs-tol
```

这些参数现在会被 benchmark 明确拒绝。

## Deferred Candidate Buffer

`frontier` 模式保留 warmup deferred SafeIn 机制：

```text
--dynamic-safein-defer-initial-clusters N
--dynamic-safein-defer-until-ready 0|1
--dynamic-safein-defer-max-candidates M
```

推荐配置：

```text
--dynamic-safein frontier
--dynamic-safein-stable-probes 1
--dynamic-safein-defer-initial-clusters 4
--dynamic-safein-defer-until-ready 1
```

触发 defer 的基础条件：

```text
dynamic_safein_probes_seen <= dynamic_safein_defer_initial_clusters
or (dynamic_safein_defer_until_ready && !dynamic_safein_ready)
```

此外，当前实现加入一个 small-pool flush gate：当 `defer_initial_clusters=4`
到点时，如果 deferred pool 小于 `max(128, 4 * submit_batch_size)`，则再让下一簇
进入 deferred pool，然后统一 flush。这样 COCO 这类候选池较小的查询保持更好的
submit batching，而 MARCO 这类候选池很大的查询仍在第 4 簇后立即 flush。

deferred buffer 保存 dedup 后候选的读地址和 SafeIn upper-bound：

```text
DeferredSafeInPlan {
    addr
    safein_upper_bound
    truth_stats_for_benchmark
}
```

当 frontier ready 或 query final drain 时，统一 flush：

```text
for plan in deferred_buffer:
    if T_q is finite
       and plan.addr.size <= safein_all_threshold
       and plan.safein_upper_bound < T_q:
        submit VEC_ALL(plan)
    else:
        submit VEC_ONLY(plan)
```

如果 query 结束时 `T_q` 仍不可用，则所有 deferred candidates 走
`VEC_ONLY`，不会丢候选。

## Readiness

`frontier` 的 ready 判断只依赖 lower frontier 是否存在以及稳定条件：

```text
has_frontier = isfinite(F_lower)
min_probe_ready = probes_seen >= dynamic_safein_min_probes
stable_ready = stable_count >= dynamic_safein_stable_probes

dynamic_safein_ready = has_frontier && min_probe_ready && stable_ready
```

稳定计数由连续 frontier 变化控制：

```text
diff = abs(F_lower - last_F_lower)
rel  = diff / max(abs(last_F_lower), 1e-12)

if diff <= dynamic_safein_abs_tol or rel <= dynamic_safein_rel_tol:
    stable_count += 1
else:
    stable_count = 1
```

推荐配置使用 `--dynamic-safein-stable-probes 1`，即 lower frontier 首次形成
即可 ready；前 4 个 cluster 通过 defer buffer 补偿早期 frontier 未形成带来
的 missed prefetch。

## 完整查询流程

```text
for each probed cluster:
    update Dynamic SafeIn state from current F_lower

    if frontier ready:
        T_q = F_lower
    else:
        T_q = disabled

    probe cluster with:
        SafeOut threshold = F_upper when Dynamic SafeOut is enabled
        SafeIn threshold  = T_q

    dedup emitted candidates

    if should_defer:
        buffer candidates as DeferredSafeInPlan
    else:
        submit SafeIn candidates as VEC_ALL
        submit remaining candidates as VEC_ONLY

    merge this cluster's estimates into F_lower/F_upper heaps
    refresh Dynamic SafeIn state without advancing probe count

    if no longer deferring and small-pool gate does not request one extra probe:
        flush deferred buffer with current T_q

after all clusters:
    force flush deferred buffer
    drain vector reads
    rerank exact vectors
    fetch missing payloads for final top-k
```

## 实验结论

COCO100k `topk=10` 的 `blend0_check` 补跑证明当前 `frontier` 语义可行：

```text
coverage   = 47.41%
false rate = 11.53%
recall@10  = 0.9457
```

最终 `bench_e2e` 验证：

```text
COCO100k frontier_defer4 + small-pool gate:
  recall@10 = 0.9457
  avg       = 0.791 ms
  p95       = 1.027 ms

MSMARCO frontier_defer4 + small-pool gate:
  recall@10 = 0.9430
  avg       = 11.290 ms
  p95       = 13.350 ms
```

因此当前正式实现选择 `frontier + defer4 + small-pool deferred flush gate`，
不再保留 `frontier_blend` 的 lambda 参数或 gap gate 调参面。

