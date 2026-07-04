# Round6：增量 bit-plane kernel 与近似 progressive pruning

日期：2026-06-28

## 目标

Round4 的 progressive active bits 方案会重复调用 `active_bits=1/2/3`，即：

- 第 1 轮计算 bit0；
- 第 2 轮重新计算 bit0+bit1；
- 第 3 轮重新计算 bit0+bit1+bit2。

本轮目标是改成真正的增量计算：

- 先计算 bit0；
- 若无法 SafeOut，再只补剩余 bit-plane；
- 在 recall 降幅不超过 5% 的前提下，验证是否能快于 fixed active=3。

## 代码修改

新增 SIMD API：

- `IPOfficialRaBitQBatchCompactTileLaneBitMajorBitDeltaMasked`
  - 只计算一个 zero-based bit-plane；
  - 写入 `(1 << bit_id) * dot(query, bitplane)`；
  - 用于维护 progressive partial IP accumulator。
- `IPOfficialRaBitQBatchCompactTileLaneBitMajorBitRangeDeltaMasked`
  - 计算连续 bit-plane range；
  - AVX512 路径支持一轮 fused 计算 bit1+bit2；
  - 用于 bits=3 时避免 bit1 和 bit2 分两次 pass。

`ClusterProber` progressive 路径更新为：

1. 只有 `safeout_frontier_upper` 有限时启用 progressive，否则直接走 fixed full Stage2。
2. 第 1 轮只计算 bit0，并用 missing-bit bound 判断 SafeOut。
3. 幸存 lane 直接用 fused range delta 补齐剩余 bit：
   - bits=2：补 bit1；
   - bits=3：补 bit1+bit2。

新增参数：

- `--stage2-progressive-missing-bound-scale`
  - 默认 `1.0`：保守 bound，理论上不应引入额外误剪；
  - 小于 `1.0`：缩小 missing-bit 区间，用于近似剪枝；
  - `0.0`：最激进，只按已计算 bit 判断。

## 验证

构建与单测：

- `cmake --build build --target test_ip_exrabitq test_cluster_prober bench_e2e -j 8`
- `./build/test_ip_exrabitq`：26/26 通过
- `./build/test_cluster_prober`：3/3 通过

新增单测：

- `OfficialTileLaneBitMajorBitDeltaAccumulatesToActiveBits`
  - 验证 bit0+bit1+bit2 增量累加等于原 fixed active=3；
  - 验证 bit0 + fused(bit1+bit2) 等于原 fixed active=3。

## 实验设置

复用索引：

- `/home/zcq/VDB/test/dynamic_exbits_stage2_20260628/indexes/amazon_esci/tile_lane_bitmajor_ex3/current_index_official_1_plus_n_total4_ex3_tile_lane_bitmajor`
- `/home/zcq/VDB/test/dynamic_exbits_stage2_20260628/indexes/msmarco_passage/tile_lane_bitmajor_ex3/current_index_official_1_plus_n_total4_ex3_tile_lane_bitmajor`

固定参数：

- `stored_ex_bits=3`, `active_ex_bits=3`, `total_bits=4`
- `topk=10`, `nprobe=64`
- `queries=1000`, `reps=5`
- `two_level_coarse_routing=1`
- `two_level_coarse_budget_factor=16`
- `non_safeout_candidate_budget=400`
- `fine_grained_timing=0`

结果目录：

- `/home/zcq/VDB/test/dynamic_exbits_stage2_20260628/runs/range_delta_reps_fixed_np64`
- `/home/zcq/VDB/test/dynamic_exbits_stage2_20260628/runs/range_delta_finite_reps_scale0_np64`

## 结果

| 数据集 | 策略 | avg ms | 相对 fixed | recall@10 | recall 相对下降 | Stage2 ms | progressive SafeOut lanes |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Amazon ESCI | fixed active=3 | 0.5465 ± 0.0173 | 0.00% | 0.7668 | 0.00% | 0.0561 | 0.000 |
| Amazon ESCI | progressive scale=0 | 0.5388 ± 0.0108 | +1.44% | 0.7612 | 0.73% | 0.0650 | 21.437 |
| MSMARCO | fixed active=3 | 1.3115 ± 0.0250 | 0.00% | 0.8101 | 0.00% | 0.1569 | 0.000 |
| MSMARCO | progressive scale=0 | 1.3209 ± 0.0419 | -0.71% | 0.8094 | 0.09% | 0.1765 | 5.454 |

补充观察：

- `scale=1.0` 保守 bound 仍然基本剪不掉 lane。
- `scale=0.0` 能产生少量剪枝，recall 降幅远小于 5%，但 Stage2 本身仍慢于 fixed active=3。
- finite-frontier gating 能显著减少无效 progressive 调用：
  - Amazon progressive round1 lanes 从约 288 降到约 56；
  - MSMARCO progressive round1 lanes 从约 751 降到约 38。

## 结论

本轮已经解决 Round4 中“重复 active=1/2/3 计算”的实现问题，但 progressive pruning 仍不适合作为最终正向优化：

1. `tile_lane_bitmajor` 的 fixed active=3 kernel 是单 pass 融合计算 bit0/bit1/bit2。
2. progressive 即使做增量，也至少需要 bit0 pass + 剩余 bit pass。
3. 当前 bit0 后的 SafeOut 比例不够高，无法抵消额外 pass 和 bound 判断开销。
4. Amazon 在 scale=0 下端到端有小幅正收益，但 Stage2 本身仍慢；MSMARCO 端到端也没有跑赢 fixed。

因此当前推荐：

- 保留增量 bit-plane kernel 作为实验能力和后续研究基础；
- 默认不把 `--stage2-progressive-active-bits` 纳入最终主方案；
- 论文中不声称 progressive pruning 已带来稳定加速；
- 若后续继续，应优先研究更强的 bound 或完全不同的早停条件，而不是继续做当前 kernel 微调。

## 后续可能方向

更可能有效的方向：

1. per-lane/per-tile missing-bit bound：利用高 bit-plane 的 popcount 或轻量摘要，缩小缺失区间。
2. block-level Stage2 envelope：在进入 Stage2 kernel 前直接判断整块是否可能 SafeOut。
3. oracle/bound-tightness 分析：统计 `min_dist - frontier`、missing interval width、true top-k 误剪风险。

不建议继续投入的方向：

- 在当前全局 query 正/负和 bound 上继续做 kernel 局部微优化；
- 在剪枝率低于 15%-20% 时强行启用 progressive。
