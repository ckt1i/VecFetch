# Round4：基于 active_ex_bits 的渐进式 Stage2 剪枝设计

日期：2026-06-28

## 目标

在 `stored_ex_bits=3` 的索引上，查询时不必一次性计算完整 3 个 Stage2 bit plane，而是按：

```text
active bit 1 -> 可剪枝则停止
active bit 2 -> 可剪枝则停止
active bit 3 -> 完整 Stage2 分类
```

逐步细化候选距离估计。目标是让 Stage2 的计算成本随查询难度动态下降，同时保持 conservative pruning，不把 partial code 当作完整距离直接剪枝。

## 关键公式

official 1+n 路径中：

```text
normalized_ip = 2^stored_ex_bits * ip_x0_qr
              + ip_ex_code
              + bias(stored_ex_bits) * sum_q

dist = query_norm_sq + factor_add + factor_rescale * norm_qc * normalized_ip
```

当只计算前 `a` 个 active bits 时：

```text
ip_ex_code = ip_active + ip_missing
```

其中 `ip_missing` 是剩余高 bit 的贡献。由于每个缺失 bit plane 的取值只可能是 0/1，可以对每个查询预先计算全局保守区间：

```text
missing_ip_min(a) = sum_{b=a}^{stored-1} 2^b * sum_i min(query[i], 0)
missing_ip_max(a) = sum_{b=a}^{stored-1} 2^b * sum_i max(query[i], 0)
```

然后将：

```text
ip_active + [missing_ip_min, missing_ip_max]
```

通过 official combine 和 distance 线性变换得到距离区间 `[dist_min, dist_max]`。若 `factor_rescale * norm_qc` 为负，区间端点需要交换。

## Conservative 分类条件

对每个 lane：

- SafeOut：`dist_min > safeout_frontier_upper + safeout_margin_s2`
- SafeIn：`dist_max < safein_threshold_base - safein_margin_s2`
- 否则继续计算下一 bit 或进入完整 Stage2 结果

其中 `dist_min/dist_max` 是考虑缺失高 bit 后的保守距离区间。因此 partial bit 只会减少计算，不会改变最终安全性判断。

## 实现策略

为了最小侵入，先只支持 `tile_lane_bitmajor`：

1. 新增查询参数，例如：
   - `--stage2-progressive-active-bits 1`
2. 在 `ClusterProber` 的 official 1+n + `tile_lane_bitmajor` 分支中，新增 progressive path。
3. progressive path 使用当前 layout 可动态选择 active bits 的能力：
   - 第 1 轮调用 kernel：`active_bits=1`
   - 对 surviving lanes 第 2 轮调用 kernel：`active_bits=2`
   - 对 surviving lanes 第 3 轮调用 kernel：`active_bits=3`
4. 每轮后根据上述距离区间更新 lane mask。
5. 统计字段补充：
   - 每轮计算的 lane 数；
   - 提前 SafeOut/SafeIn 的 lane 数；
   - 平均实际 active bits。

## 风险

- `active_bits=1/2` 每轮重新调用当前 kernel，会重复计算低 bit；因此第一版可能不一定加速。
- 若缺失高 bit 区间过宽，前两轮可剪枝比例可能偏低。
- 若要进一步优化，需要 kernel 支持“增量计算 bit plane”：第 2 轮只补 bit1，第 3 轮只补 bit2，而不是重算前面的 bit。

## 验证口径

先做一轮 smoke：

- 数据集：Amazon ESCI
- layout：`tile_lane_bitmajor`
- `stored_ex_bits=3`
- `topk=10`
- `nprobe=64`
- `queries=1000`

对比：

| 策略 | 说明 |
| --- | --- |
| fixed active=3 | 当前完整 Stage2 |
| progressive 1->2->3 | 新增渐进式剪枝 |

主要指标：

- recall@10 不得下降；
- avg query ms；
- avg Stage2 ms；
- 每轮实际处理 lane 数；
- avg effective active bits。

## Amazon Smoke 结果

运行口径：

- `queries=1000`
- `topk=10`
- `nprobe=64`
- `stored_ex_bits=3`
- `active_ex_bits=3`
- layout：`tile_lane_bitmajor`

| 策略 | avg ms | Stage2 ms | recall@10 | round1 lanes | round2 lanes | round3 lanes | progressive safeout |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| fixed active=3 | 0.5099 | 0.0507 | 0.7668 | 0.0 | 0.0 | 0.0 | 0.0 |
| progressive 1->2->3 | 0.5629 | 0.0895 | 0.7668 | 286.4 | 286.4 | 286.4 | 0.0 |

## 结论

第一版 conservative progressive SafeOut 为负结果：

- partial bit 后的缺失高 bit 区间过宽，`active=1/2` 阶段没有产生提前 SafeOut；
- 当前实现为了保持低风险，调用现有 kernel 重新计算 `active=1`、`active=2`、`active=3`，导致 Stage2 lanes 统计从 286.4 变成 859.2；
- avg Stage2 从 0.0507 ms 增加到 0.0895 ms，总查询时间也变慢。

因此该策略不进入最终方案。若后续继续做动态剪枝，需要先实现“增量 bit-plane kernel”（第 2 轮只补 bit1，第 3 轮只补 bit2），并且需要比全局 query 正负和更紧的 per-tile/per-lane missing bound，否则 conservative pruning 不会产生收益。
