# Auto Review Loop: Dynamic ExBits Stage2

本文件是本轮 `$auto-review-loop` 的累计记录。Codex MCP reviewer 如果不可用，则使用本地审查和可用的外部子代理替代，但必须保留每轮行动、结果和失败信息。

## Round 0 初始化

- 目标：拆分 `stored_ex_bits` / `active_ex_bits`，并围绕 `vector_bitmajor_tiles` 验证动态降级和格式优化。
- 固定口径：复用 no-combine ablation 的 Amazon ESCI / MSMARCO 参数。
- 初始设计：见 `DESIGN_ROUND0_CN.md`。
- 当前状态：已完成基础实现与 Amazon smoke。

## Round 0 执行结果：stored/active 拆分

代码改动：

- `SearchConfig` 增加查询期 `rabitq_active_ex_bits`。
- `bench_e2e` 增加 `--rabitq-active-ex-bits`，结果 JSON 的 `metrics` 中记录：
  - `rabitq_stored_ex_bits`
  - `rabitq_active_ex_bits`
- Stage2 direct kernels 使用 `stored_ex_bits` 计算物理 stride，使用 `active_ex_bits` 控制实际 bit-plane 计算。
- `pipeline_stats` 增加：
  - `avg_stage2_active_ex_bits`
  - `avg_stage2_stored_ex_bits`
  - `avg_stage2_lanes_requested`

验证：

- `cmake --build build --target test_ip_exrabitq -j4`
- `./build/test_ip_exrabitq`：25/25 通过。
- `./build/test_cluster_store`：25/25 通过。
- `./build/test_cluster_prober`：3/3 通过。
- `./build/test_types`：32/32 通过。

Amazon smoke 索引：

- `/home/zcq/VDB/test/dynamic_exbits_stage2_20260628/indexes/amazon_esci/vector_bitmajor_tiles_ex3/current_index_official_1_plus_n_total4_ex3_vector_bitmajor_tiles`
- 构建复用：
  - `/home/zcq/VDB/test/rabitq_fair_ex3_20260624/artifacts/amazon_esci/centroids.fvecs`
  - `/home/zcq/VDB/test/rabitq_fair_ex3_20260624/artifacts/amazon_esci/assignments.ivecs`

Smoke 结果，`queries=50, topk=10, nprobe=64, two-level factor=16`：

| layout | active_ex_bits | avg ms | recall@10 | avg_stage2_active_ex_bits | avg_stage2_stored_ex_bits |
| --- | ---: | ---: | ---: | ---: | ---: |
| vector_bitmajor_tiles | 1 | 0.5928 | 0.5360 | 1.0 | 3.0 |
| vector_bitmajor_tiles | 3 | 0.7247 | 0.8960 | 3.0 | 3.0 |

## Round 1 执行结果：tile_lane_bitmajor

设计：见 `ROUND1_TILE_LANE_BITMAJOR_CN.md`。

代码改动：

- 新增 layout：`tile_lane_bitmajor`。
- 存储格式：`[tile][bit][lane][tile_dims/8]`，同一 batch block 内跨 lane 重排 Stage2 ExData。
- 新增 SIMD API：
  - `ExRaBitQPackOfficialTileLaneBitMajor`
  - `ExRaBitQUnpackOfficialTileLaneBitMajor`
  - `IPOfficialRaBitQBatchCompactTileLaneBitMajorMasked`
- prober dispatch 支持该 layout。

验证：

- `./build/test_ip_exrabitq`：25/25 通过，覆盖 pack/unpack 与 partial active bits。
- `./build/test_cluster_store`：25/25 通过，参数化覆盖 `ex_bits=1/2/3`。
- `./build/test_cluster_prober`：3/3 通过，参数化覆盖 `ex_bits=1/2/3`。

Amazon smoke 索引：

- `/home/zcq/VDB/test/dynamic_exbits_stage2_20260628/indexes/amazon_esci/tile_lane_bitmajor_ex3/current_index_official_1_plus_n_total4_ex3_tile_lane_bitmajor`

Smoke 结果，同上口径：

| layout | active_ex_bits | avg ms | recall@10 | 对 vector_bitmajor_tiles |
| --- | ---: | ---: | ---: | ---: |
| tile_lane_bitmajor | 1 | 0.5577 | 0.5360 | +6.28% |
| tile_lane_bitmajor | 3 | 0.6463 | 0.8960 | +12.39% |

判断：

- 新 layout 在 Amazon smoke 下保持 recall 完全一致，并且 active=1/3 都更快。
- 需要进入 formal 对比：Amazon/MSMARCO，`queries=1000`，对照 `official_vector_bitplanes`，使用 no-combine ablation 同一参数。

## Auto Review Round 1

外部子代理只读审查结论：

- `active_ex_bits < stored_ex_bits` 不是重新量化的低 bit RaBitQ；若用于分类，语义应明确为 partial stored-code approximation。
- 部分 official layout 没有实现 active-aware kernel，但此前统计字段会显示 active 已生效。
- v15 variable ExData block view 对 layout-specific payload 长度缺少显式边界校验。
- JSON 字段需要区分配置位宽、实际支持情况和执行口径。

已处理：

- 增加 `RaBitQExDataLayoutSupportsActiveExBits`，明确哪些 layout 支持 partial active bits。
- `bench_e2e` 在 `active_ex_bits > 0 && active_ex_bits < stored_ex_bits` 时拒绝不支持该语义的 layout。
- 结果 JSON 保留旧字段，同时新增：
  - `rabitq_configured_active_ex_bits`
  - `rabitq_effective_active_ex_bits`
  - `rabitq_layout_honors_active_ex_bits`
  - `rabitq_active_ex_bits_mode`
- `ParsedCluster` 增加内存字段 `dim`，并对 `vector_bitmajor_tiles` / `tile_lane_bitmajor` 的 v15 variable ExData payload 做 layout-specific 边界校验。
- `ResidentClusterView` 同步保存/copy `dim`，保证 resident preload 路径也能执行同样校验。
- `TypesTest` 增加 active bits support helper 覆盖。

验证：

- `cmake --build build --target bench_e2e test_ip_exrabitq test_cluster_store test_cluster_prober test_types -j4`
- `./build/test_ip_exrabitq`：25/25 通过。
- `./build/test_cluster_store`：25/25 通过。
- `./build/test_cluster_prober`：3/3 通过。
- `./build/test_types`：33/33 通过。
- JSON smoke：`vector_bitplanes, stored=3, active=1` 输出 `mode=partial_stored_code` 且 `rabitq_layout_honors_active_ex_bits=true`。

剩余说明：

- partial active bits 仍不等价于真正的低 bit RaBitQ；后续动态剪枝若要维持安全性，需要单独设计 conservative bound，而不能直接把 partial score 当 full-score 安全替代。

## Round 2 执行结果：tile kernel fusion

设计：见 `ROUND2_TILE_KERNEL_FUSION_CN.md`。

修改：

- `tile_lane_bitmajor` 的 AVX512 hot path 从按 bit-plane 外层循环改成按 query slice 外层循环。
- 对同一 query slice，一次 load 后分别累积 bit0/bit1/bit2，最终以 `1/2/4` 权重合成。
- 存储格式不变，因此复用 Round1 `tile_lane_bitmajor` 索引。

Amazon 50-query 自检，`topk=10,nprobe=64,active=3`：

| layout | avg ms | recall@10 | 备注 |
| --- | ---: | ---: | --- |
| vector_bitplanes | 0.6319 | 0.8960 | official-like baseline，同二进制重跑 |
| tile_lane_bitmajor | 0.6367 | 0.8960 | Round2 fused kernel |

判断：

- Round2 相比 Round1 `tile_lane_bitmajor` 的 0.6502 ms 有小幅改善。
- 仍未超过 `vector_bitplanes` 2%，因此需要继续 Round3 优化或调整策略。

## Round 3 执行结果：tile pointer hoist

设计：见 `ROUND3_TILE_POINTER_HOIST_CN.md`。

修改：

- 尝试在每个 tile 开始时提前计算 requested lanes 的 bit-plane 指针，减少内层循环中的 `lane * plane_bytes` 地址计算。

验证：

- `test_ip_exrabitq`：25/25 通过。
- `test_cluster_store`：25/25 通过。
- `test_cluster_prober`：3/3 通过。

Amazon 50-query smoke：

| layout | avg ms | Stage2 ms | recall@10 |
| --- | ---: | ---: | ---: |
| vector_bitplanes | 0.6313 | 0.0622 | 0.8960 |
| tile_lane_bitmajor + pointer hoist | 0.6411 | 0.0627 | 0.8960 |

判断：

- 该优化为负收益，已从代码中撤回。
- 可能原因是临时指针数组增加寄存器压力/栈访问，而原地址计算已被编译器较好优化。

## Formal Sweep：fixed active=3

结果文档：见 `FORMAL_MIN_SWEEP_RESULTS_CN.md`。

口径：

- Amazon ESCI / MSMARCO Passage
- `stored_ex_bits=3`
- `active_ex_bits=3`
- `topk=10`
- `nprobe=64,128,256,512`
- `queries=1000`
- baseline：`vector_bitplanes`
- new：`tile_lane_bitmajor`

结论：

- `tile_lane_bitmajor` 在 Stage2 上有稳定收益：
  - Amazon：约 1.9%-7.3% Stage2 加速；
  - MSMARCO：约 4.3%-4.9% Stage2 加速。
- 端到端总查询没有稳定达到 2%：
  - Amazon：-1.06% 到 +0.35%；
  - MSMARCO：最好 +1.09%，其余点略慢。

审查判断：

- 当前实现可以支持“Stage2 码布局降低量化码扫描成本”的弱 claim。
- 当前证据不支持“稳定 2% 端到端加速”的强 claim。

## Round 4 执行结果：progressive active bits pruning

设计：见 `ROUND4_DYNAMIC_EXBITS_PRUNING_CN.md`。

修改：

- 新增 `SearchConfig::enable_stage2_progressive_active_bits`。
- `bench_e2e` / `bench_online_query` 增加 `--stage2-progressive-active-bits`。
- 对 `tile_lane_bitmajor` 增加 conservative SafeOut-only progressive path：
  - 第 1 轮：`active_bits=1`
  - 第 2 轮：`active_bits=2`
  - 第 3 轮：`active_bits=3`
- partial 轮次只在缺失高 bit 的保守距离下界已经超过 SafeOut frontier 时提前剪枝。

Amazon 1000-query smoke：

| 策略 | avg ms | Stage2 ms | recall@10 | round1 lanes | round2 lanes | round3 lanes | progressive safeout |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| fixed active=3 | 0.5099 | 0.0507 | 0.7668 | 0.0 | 0.0 | 0.0 | 0.0 |
| progressive 1->2->3 | 0.5629 | 0.0895 | 0.7668 | 286.4 | 286.4 | 286.4 | 0.0 |

验证：

- `cmake --build build --target bench_e2e test_ip_exrabitq test_cluster_prober -j4` 通过。
- `./build/test_ip_exrabitq`：25/25 通过。
- `./build/test_cluster_prober`：3/3 通过。
- `./build/benchmarks/bench_e2e --help` 已暴露 `--stage2-progressive-active-bits`。

判断：

- Conservative progressive pruning 为负结果，不进入最终方案。
- 主要原因是全局缺失高 bit 区间太宽，partial 阶段没有提前 SafeOut；当前实现还重复计算低 bit，导致 Stage2 lanes 从 286.4 增至 859.2。
- 若后续继续该方向，需要增量 bit-plane kernel 和更紧的 per-tile/per-lane missing bound。

## Auto Review Round 2

外部子代理只读审查结论：

- 当前结果能作为第二贡献的一部分，但只能是弱贡献/子贡献。
- 可辩护表述是：
  - `stored_ex_bits` / `active_ex_bits` 解耦；
  - 同一 `stored_ex_bits=3` 索引支持查询期选择较低 `active_ex_bits`；
  - `tile_lane_bitmajor` 在 Stage2 code-scan/kernel 上稳定降低成本。
- 不可辩护表述是：
  - 新 Stage2 格式稳定带来 2% 端到端加速；
  - progressive active-bit pruning 已加速 SafeOut。
- 如果继续，最低成本补证据不是继续写复杂 kernel，而是：
  - 对 formal sweep 做多 rep，报告方差；
  - 补齐 `topk=100`；
  - 对 `active_ex_bits=1/2/3` 做正式 1000-query speed/recall sweep；
  - 对 progressive pruning 做离线 bound tightness/oracle 分析。

本轮采纳：

- 不再把 Stage2 layout 单独追 2% 端到端作为当前最优方向；
- 先补 `topk=100` 与 active bits sweep，用于支撑更稳妥的论文表述。

## Round 5 执行结果：8-lane hot path 收缩

设计：见 `ROUND5_TILE_LANE8_STACK_TRIM_CN.md`。

修改：

- 尝试将 `tile_lane_bitmajor` AVX512 hot path 中的临时数组从 32 lane 缩为 8 lane。

验证：

- `test_ip_exrabitq`：25/25 通过。
- `test_cluster_prober`：3/3 通过。

结果：

- Amazon：tile avg 从旧版 0.5065 ms 变为 0.5136 ms，Stage2 从 0.0501 ms 变为 0.0518 ms。
- MSMARCO：tile avg 从旧版 1.2540 ms 变为 1.2632 ms，Stage2 从 0.1525 ms 变为 0.1549 ms。

判断：

- 该优化为负收益，已从代码撤回。
- 这进一步说明当前端到端 2% 缺口不在简单的 tile-lane kernel 局部开销上。

## Round 6 执行结果：增量 bit-plane progressive pruning

设计与详细结果：见 `ROUND6_INCREMENTAL_BITPLANE_PRUNING_CN.md`。

外部自审要点：

- 仅消除重复 active=1/2/3 计算不足以保证 progressive 跑赢 fixed active=3。
- 当前 missing-bit bound 过宽；若 `progressive_safeout_lanes` 接近 0，应停止 kernel 微调，转向 bound 质量分析。
- 最小门槛应同时看 recall、Stage2 ms、端到端 avg ms、每轮 lanes、false SafeOut。

本轮修改：

- 新增 `IPOfficialRaBitQBatchCompactTileLaneBitMajorBitDeltaMasked`。
- 新增 `IPOfficialRaBitQBatchCompactTileLaneBitMajorBitRangeDeltaMasked`。
- progressive 路径改为：
  - bit0 后做一次 SafeOut；
  - 幸存者用 fused bit1+bit2 range delta 补齐；
  - 仅在 `safeout_frontier_upper` 有限时启用 progressive。
- 新增 `--stage2-progressive-missing-bound-scale`，用于近似剪枝 sweep。

验证：

- `cmake --build build --target test_ip_exrabitq test_cluster_prober bench_e2e -j 8` 通过。
- `./build/test_ip_exrabitq`：26/26 通过。
- `./build/test_cluster_prober`：3/3 通过。

Reps=5 smoke 结果，`topk=10,nprobe=64,queries=1000`：

| 数据集 | 策略 | avg ms | recall@10 | Stage2 ms | progressive SafeOut lanes |
| --- | --- | ---: | ---: | ---: | ---: |
| Amazon ESCI | fixed active=3 | 0.5465 ± 0.0173 | 0.7668 | 0.0561 | 0.000 |
| Amazon ESCI | progressive scale=0 | 0.5388 ± 0.0108 | 0.7612 | 0.0650 | 21.437 |
| MSMARCO | fixed active=3 | 1.3115 ± 0.0250 | 0.8101 | 0.1569 | 0.000 |
| MSMARCO | progressive scale=0 | 1.3209 ± 0.0419 | 0.8094 | 0.1765 | 5.454 |

判断：

- 增量 kernel 已实现并验证正确，Round4 的重复计算问题已解决。
- 当前 progressive pruning 仍不能作为最终优化：Amazon 端到端小幅正收益但 Stage2 变慢，MSMARCO 端到端也未跑赢 fixed。
- 默认仍不启用 progressive；保留代码作为后续 bound/oracle 分析基础。

## Round 7 执行结果：bound 放松与 active bits fallback

设计与详细结果：见 `ROUND7_BOUND_RELAXATION_CN.md`。

本轮修改：

- 新增 `--stage2-progressive-safeout-rel-slack`。
- 当 rel slack 大于 0 时，允许：
  - progressive partial bound 以 `frontier * rel_slack` 放松 SafeOut；
  - S1-relaxed early SafeOut 直接剪掉部分接近 frontier 的 S2 候选；
  - S1 gate 只让更可能被剪掉的 lane 进入 progressive。
- 默认 `rel_slack=0` 不改变 Round6 progressive 语义。

验证：

- `cmake --build build --target test_cluster_prober bench_e2e -j 8` 通过。
- `./build/test_cluster_prober`：3/3 通过。
- `slack=0` sanity：Amazon `avg_ms=0.528792`, `recall@10=0.7612`, `round1_lanes=55.705`, `progressive_safeout=21.437`。

active bits fallback 结论：

- Amazon active=2：recall 相对下降 6.61%，不满足 5% 限制。
- MSMARCO active=2：recall 相对下降 10.88%，不满足 5% 限制。
- active=1 更差，因此固定少算 bit-plane 不能作为最终 fallback。

bound slack sweep 结论：

- `rel_slack=0.02` recall 下降仍在 5% 内：
  - Amazon：`avg_ms=0.6869`, `recall@10=0.7461`
  - MSMARCO：`avg_ms=1.6512`, `recall@10=0.7860`
  - 但两者都明显慢于 fixed。
- 更大的 slack 能剪更多 lane，但 recall 迅速超过 5% 限制。

判断：

- 当前 bound 放松方向没有找到合格点。
- 继续追 progressive 需要新的 per-lane/tile 摘要或 oracle 分析，而不是继续放松当前全局 bound。
- 最终方案仍建议保留格式优化与动态 active bits 能力，但默认关闭 progressive pruning。

## Round 8 执行结果：full Stage2 SafeOut oracle 上限

设计与详细结果：见 `ROUND8_ORACLE_UPPER_BOUND_CN.md`。

目的：

- 不再继续盲调 progressive 参数；
- 用 fixed active=3 的实际 Stage2 分类结果估计理论可提前剪枝上限。

关键统计，`topk=10,nprobe=64,queries=1000,reps=5`：

| 数据集 | fixed Stage2 lanes | fixed full S2 SafeOut | SafeOut 占比 | progressive scale=0 early SafeOut |
| --- | ---: | ---: | ---: | ---: |
| Amazon ESCI | 286.400 | 30.846 | 10.8% | 21.437 |
| MSMARCO | 750.986 | 17.342 | 2.3% | 5.454 |

判断：

- Amazon 的 full S2 SafeOut 上限也只有约 10.8%，低于理想 15%-20% 剪枝门槛。
- MSMARCO 的 full S2 SafeOut 只有约 2.3%，即使 bound 完美也很难抵消 progressive 的额外 pass 和控制开销。
- 因此当前失败不只是 bound 公式松，而是当前口径下 Stage2 内可剪 lane 总量太少。

最终结论：

- 保留增量 bit-plane kernel 和动态 active bits 支持；
- 默认关闭 `--stage2-progressive-active-bits`；
- 不把 progressive pruning 写成最终主方案；
- 若后续继续，需要改变问题层级：Stage1/block-level envelope、候选进入 Stage2 的条件，或在不同 topk/nprobe/budget 下重新评估。
