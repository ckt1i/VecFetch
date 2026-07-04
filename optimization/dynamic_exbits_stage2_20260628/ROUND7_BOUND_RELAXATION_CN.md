# Round7：bound 放松与 active bits fallback 复查

日期：2026-06-28

## 背景

Round6 已经实现增量 bit-plane kernel，并把 progressive 从重复计算
`active=1/2/3` 改为：

1. bit0 delta；
2. bit0 后 SafeOut；
3. 幸存者 fused 计算 bit1+bit2。

但结果显示：当前 conservative/scale=0 bound 的剪枝率不足，Stage2 自身仍慢于 fixed active=3。本轮继续检查两个方向：

1. 固定少算 active bits 是否可作为 fallback；
2. 放松 progressive SafeOut bound 是否能在 recall 下降 5% 内跑赢 fixed。

## 修改

新增参数：

- `--stage2-progressive-safeout-rel-slack`
  - 默认 `0.0`，不改变 Round6 默认语义；
  - 大于 0 时，允许用 `frontier * rel_slack` 放松 SafeOut 判断。

实现细节：

- progressive partial bound：
  - 原条件：`min_dist > frontier + margin`
  - 放松后：`min_dist + frontier * rel_slack > frontier + margin`
- S1-relaxed early SafeOut：
  - 仅当 `rel_slack > 0` 时启用；
  - 用 `est_dist_s1 - safeout_margin_s1 + frontier * rel_slack > frontier` 直接剪掉部分 S2 候选；
  - 目的是避免为明显偏远的 S2 候选再做 bit0 pass。
- S1 gate：
  - 仅当 `rel_slack > 0` 时启用；
  - 只有 `est_dist_s1 + frontier * rel_slack > frontier` 的 lane 进入 progressive；
  - 其余 lane 直接走 fixed full Stage2。

## 验证

- `cmake --build build --target test_cluster_prober bench_e2e -j 8` 通过。
- `./build/test_cluster_prober`：3/3 通过。
- `slack=0` sanity：
  - Amazon `avg_ms=0.528792`
  - `recall@10=0.7612`
  - `round1_lanes=55.705`
  - `progressive_safeout=21.437`
  - 与 Round6 slack=0 语义一致。

## 固定 active bits fallback

设置：

- `topk=10,nprobe=64,queries=1000,reps=5`
- `STAGE2_PROGRESSIVE=0`
- 复用 `tile_lane_bitmajor_ex3` 索引。

| 数据集 | 策略 | avg ms | 相对 fixed a3 | recall@10 | recall 相对下降 | Stage2 ms |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Amazon ESCI | fixed active=3 | 0.5465 ± 0.0173 | 0.00% | 0.7668 | 0.00% | 0.0561 |
| Amazon ESCI | fixed active=2 | 0.5704 ± 0.0460 | -4.18% | 0.7161 | 6.61% | 0.0475 |
| Amazon ESCI | fixed active=1 | 0.5494 ± 0.0438 | -0.53% | 0.6797 | 11.36% | 0.0409 |
| MSMARCO | fixed active=3 | 1.3115 ± 0.0250 | 0.00% | 0.8101 | 0.00% | 0.1569 |
| MSMARCO | fixed active=2 | 1.2767 ± 0.0584 | +2.73% | 0.7220 | 10.88% | 0.1356 |
| MSMARCO | fixed active=1 | 1.3425 ± 0.0722 | -2.31% | 0.7051 | 12.96% | 0.1381 |

结论：固定 `active_ex_bits=1/2` 不能作为合格 fallback；两个数据集至少一个 recall 相对下降超过 5%。

## S1-relaxed / bound slack sweep

设置：

- `topk=10,nprobe=64,queries=1000,reps=1`
- `STAGE2_PROGRESSIVE=1`
- `STAGE2_PROGRESSIVE_BOUND_SCALE=0.0`
- sweep `STAGE2_PROGRESSIVE_SAFEOUT_REL_SLACK={0.02,0.05,0.10,0.20}`。

| 数据集 | rel slack | avg ms | recall@10 | recall 相对下降 | Stage2 ms | progressive SafeOut | round1 lanes | total Stage2 lanes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Amazon ESCI | 0.02 | 0.6869 | 0.7461 | 2.70% | 0.0737 | 34.968 | 27.867 | 281.759 |
| Amazon ESCI | 0.05 | 0.5834 | 0.6882 | 10.25% | 0.0690 | 53.231 | 15.829 | 259.187 |
| Amazon ESCI | 0.10 | 0.5681 | 0.5783 | 24.58% | 0.0721 | 101.293 | 4.417 | 240.425 |
| Amazon ESCI | 0.20 | 0.5348 | 0.5005 | 34.73% | 0.0749 | 139.146 | 0.072 | 232.751 |
| MSMARCO | 0.02 | 1.6512 | 0.7860 | 2.97% | 0.2217 | 17.243 | 19.233 | 753.788 |
| MSMARCO | 0.05 | 1.4012 | 0.7414 | 8.48% | 0.1830 | 30.247 | 10.912 | 735.310 |
| MSMARCO | 0.10 | 1.3667 | 0.6669 | 17.68% | 0.1774 | 54.189 | 3.508 | 719.858 |
| MSMARCO | 0.20 | 1.3261 | 0.5785 | 28.59% | 0.1801 | 79.702 | 0.076 | 713.111 |

结论：

- `rel_slack=0.02` 的 recall 下降仍在 5% 内，但两个数据集都明显变慢。
- 更大的 slack 会带来更多剪枝，但 recall 迅速超过 5% 限制。
- S1-relaxed early SafeOut 能减少 Stage2 lanes，但节省不够抵消额外控制开销和查询质量损失。

## 当前判断

本轮进一步支持 Round6 的判断：

1. 增量 bit-plane kernel 是正确的，但 progressive pruning 不是当前最优路径。
2. 目前的问题不是“active=1/2/3 重复计算”，而是可安全或近似安全剪掉的 lane 数不够。
3. 固定少算 active bits 会造成过大 recall 损失。
4. 放松 bound 可以剪更多，但要么不快，要么 recall 超过 5% 限制。

因此，当前最终实现建议：

- 保留增量 bit-plane kernel 和调参入口，作为实验/后续研究能力；
- 默认关闭 `--stage2-progressive-active-bits`；
- 不把 progressive pruning 作为 topk=10 最终主方案；
- 论文中可以写“格式支持动态 active bits 和 progressive 计算”，但不能声称该 progressive 策略已经稳定加速。

## 后续若必须继续

若仍要让 progressive 成为正向结果，需要新的信息，而不是继续放松当前 bound：

1. 离线 oracle：统计如果只看 bit0，哪些 lane 实际 full Stage2 后会 SafeOut。
2. per-lane/tile 摘要：为 high bit-plane 建轻量摘要，让 missing bound 真正变紧。
3. query-adaptive 策略：只在观测到 `frontier` 足够稳定、S2 候选密度高且预计 prune ratio 高时启用 progressive。
