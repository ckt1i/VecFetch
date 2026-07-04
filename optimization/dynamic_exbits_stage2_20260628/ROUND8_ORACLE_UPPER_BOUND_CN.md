# Round8：基于 full Stage2 SafeOut 的 oracle 上限分析

日期：2026-06-28

## 目的

Round6 和 Round7 已经验证：

- 增量 bit-plane kernel 正确；
- 放松 bound 可以剪更多 lane，但速度/recall 不能同时达标；
- 固定少算 active bits 的 recall 损失过大。

本轮不再继续盲调参数，而是用已有 fixed active=3 结果估计 progressive pruning 的理论上限：如果有一个理想 bound，它最多也只能提前剪掉 full Stage2 最终会判为 SafeOut 的 lane。

## 使用结果

复用结果目录：

- fixed active=3：
  `/home/zcq/VDB/test/dynamic_exbits_stage2_20260628/runs/range_delta_reps_fixed_np64`
- progressive scale=0：
  `/home/zcq/VDB/test/dynamic_exbits_stage2_20260628/runs/range_delta_finite_reps_scale0_np64`

参数：

- `topk=10`
- `nprobe=64`
- `queries=1000`
- `reps=5`
- `stored_ex_bits=3`
- `active_ex_bits=3`

## 关键统计

| 数据集 | 策略 | Stage2 lanes | full/progressive S2 SafeOut | progressive round1 lanes | progressive early SafeOut |
| --- | --- | ---: | ---: | ---: | ---: |
| Amazon ESCI | fixed active=3 | 286.400 | 30.846 | 0.000 | 0.000 |
| Amazon ESCI | progressive scale=0 | 322.580 | 36.616 | 55.705 | 21.437 |
| MSMARCO | fixed active=3 | 750.986 | 17.342 | 0.000 | 0.000 |
| MSMARCO | progressive scale=0 | 783.791 | 18.342 | 38.143 | 5.454 |

解释：

- Amazon fixed active=3 中，full Stage2 平均只有约 `30.846 / 286.400 = 10.8%` lane 最终 SafeOut。
- MSMARCO fixed active=3 中，full Stage2 平均只有约 `17.342 / 750.986 = 2.3%` lane 最终 SafeOut。
- progressive scale=0 已经在 Amazon 提前剪掉 `21.437` lane，约等于 fixed full SafeOut 上限的 `69.5%`。
- progressive scale=0 在 MSMARCO 提前剪掉 `5.454` lane，约等于 fixed full SafeOut 上限的 `31.4%`，但 full SafeOut 总量本身只有 2.3%。

## 为什么 bound 优化空间有限

progressive 的额外成本来自：

1. 对进入 progressive 的 lane 先做 bit0 pass；
2. 之后对幸存 lane 做 fused bit1+bit2 pass；
3. 还要做 mask、bound、scatter 控制逻辑。

因此它必须提前剪掉足够多 lane 才能跑赢 fixed active=3 的单 pass fused kernel。

但 full Stage2 oracle 上限显示：

- Amazon 最多只有约 10.8% lane 可被 full Stage2 SafeOut；
- MSMARCO 最多只有约 2.3% lane 可被 full Stage2 SafeOut；
- MSMARCO 的理论可剪空间远低于此前外部自审建议的 15%-20% 门槛。

这说明问题不只是“当前 bound 太松”，而是当前实验口径下 Stage2 SafeOut 本身就很少。即使拥有完美 bound，progressive 也很难抵消多 pass 和控制开销。

## 与 Round6/Round7 的关系

Round6：

- 解决了重复计算低 bit 的问题；
- Amazon `scale=0` 有小幅端到端收益，但 Stage2 仍慢；
- MSMARCO 未跑赢 fixed。

Round7：

- 放松 bound 后能剪更多 lane；
- 但 recall 很快超过 5% 限制；
- 在 recall 下降 5% 内的点仍然慢。

Round8 oracle 上限解释了原因：

- 可提前剪的 lane 总量太少；
- 继续改当前 missing-bit bound 的收益上限有限；
- 真正需要的是改变候选进入 Stage2 的条件，或在 Stage1/submit/rerank 层面减少工作，而不是继续优化 Stage2 内 progressive。

## 结论

在当前 topk=10、nprobe=64、two-level routing、budget=400 的口径下，progressive bit-plane pruning 不应作为最终主方案：

- 增量 bit-plane kernel 可以保留；
- 动态 active bits 能力可以保留；
- `--stage2-progressive-active-bits` 默认应保持关闭；
- 论文中不能声称 progressive pruning 稳定加速。

## 若后续必须继续

继续推进需要改变问题定义，而不是继续调当前 bound：

1. 在 Stage1 前后做 block-level oracle/envelope，减少进入 Stage2 的候选总量。
2. 设计 per-lane/tile 高 bit 摘要，但只有在 full Stage2 SafeOut 比例更高的口径下才值得。
3. 在更大 topk、更大 nprobe、不同 budget 下重新评估 full Stage2 SafeOut 比例；若比例仍低，progressive 没有系统收益空间。
