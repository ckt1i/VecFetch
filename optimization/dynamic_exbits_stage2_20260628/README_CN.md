# Dynamic ExBits Stage2 优化记录

日期：2026-06-28

本目录记录本轮围绕 RaBitQ Stage2 格式与动态 `ex_bits` 的实现、优化、评审和实验。实验输出统一放在：

`/home/zcq/VDB/test/dynamic_exbits_stage2_20260628`

复用实验口径来自：

`/home/zcq/VDB/paper/experiment-documents/no-combine-ablation-amazon-msmarco-ex3-20260627`

## 目标

1. 将索引构建期的 `ex_bits` 与查询期的 `ex_bits` 拆分：
   - `stored_ex_bits`：索引中实际存储的 Stage2 extra-bit 宽度。
   - `active_ex_bits`：单次查询实际计算的 Stage2 extra-bit 宽度。
2. 继续使用当前按 bit-plane/tile 组织的方案，验证其在 `stored_ex_bits=3` 下相比 official-like RaBitQ 格式是否能在 `amazon_esci` 与 `msmarco_passage` 都达到至少 2% 加速。
3. 至少做三轮格式/布局优化，每轮记录设计、命令、结果与保留/回退决策。
4. 至少做一轮动态 `active_ex_bits` 剪枝策略：Stage2 可以先用较低 bit 宽度做初筛，再按需要追加更高 bit 并重新剪枝。
5. 优化结束后清理中间版本索引，只保留：
   - 最优一版我们的方法索引。
   - 原始 RaBitQ/official-like baseline 索引。

## 固定实验口径

- 数据集：`amazon_esci`, `msmarco_passage`
- `topk={10,100}`
- `nprobe={64,128,256,512}`，快速 smoke 可先用代表点。
- `stored_ex_bits=3`, `total_bits=4`
- `two_level_coarse_routing=1`
- `two_level_coarse_budget_factor=16`
- `non_safeout_candidate_budget=400`
- `dynamic_safeout=1`
- `dynamic_safein=frontier`
- `io_queue_depth=64`
- `fixed_vec_buffer_count=1024`
- `fine_grained_timing=0`
- `hotpath_detailed_timing=0`
- 正式查询数：`queries=1000`

## Baseline 索引

原始 official-like baseline 使用 No Combine 文档中的 official vector-bitplanes 索引：

- Amazon ESCI：
  `/home/zcq/VDB/test/rabitq_fair_ex3_20260624/indexes/amazon_esci/official_vector_bitplanes/current_index_official_1_plus_n_total4_ex3_vector_bitplanes`
- MSMARCO Passage：
  `/home/zcq/VDB/test/rabitq_fair_ex3_20260624/indexes/msmarco_passage/official_vector_bitplanes/current_index_official_1_plus_n_total4_ex3_vector_bitplanes`

我们的方法优先复用现有 `vector_bitmajor_tiles` 索引；若存储格式变更需要重建，则新索引仅写入本轮 test 目录。

## 当前状态

截至 2026-06-28，本轮已经完成：

- `stored_ex_bits` / `active_ex_bits` 拆分；
- 新增 `tile_lane_bitmajor` Stage2 layout；
- 三轮格式优化：
  - Round1：新增 batch tile/lane/bit-major 存储；
  - Round2：融合 bit-plane kernel，保留；
  - Round3：tile 内指针提升，负收益，撤回；
- Amazon/MSMARCO `queries=1000` fixed active=3 sweep；
- 一轮 conservative progressive active bits pruning，负收益，默认关闭。
- 一轮增量 bit-plane progressive pruning：
  - 已实现单 bit-plane delta kernel 与 fused bit1+bit2 range delta kernel；
  - 解决了 Round4 重复计算低 bit 的问题；
  - 但当前 bound 剪枝率仍不足，Stage2 本身仍慢于 fixed active=3；
  - 详见 `ROUND6_INCREMENTAL_BITPLANE_PRUNING_CN.md`。
- 一轮 bound 放松与 active bits fallback 复查：
  - 固定 `active_ex_bits=1/2` 的 recall 相对下降超过 5%，不能作为主 fallback；
  - `stage2_progressive_safeout_rel_slack` 可以剪更多 lane，但要么不快，要么 recall 超过 5% 限制；
  - 详见 `ROUND7_BOUND_RELAXATION_CN.md`。
- 一轮 full Stage2 SafeOut oracle 上限分析：
  - Amazon fixed active=3 中 full Stage2 SafeOut 约 10.8%；
  - MSMARCO fixed active=3 中 full Stage2 SafeOut 约 2.3%；
  - 说明当前口径下可提前剪枝的 lane 总量本身不足；
  - 详见 `ROUND8_ORACLE_UPPER_BOUND_CN.md`。

当前证据：

- `tile_lane_bitmajor` 相比 `vector_bitplanes` 在 Stage2 上有稳定收益：
  - Amazon：约 1.9%-7.3% Stage2 加速；
  - MSMARCO：约 4.3%-4.9% Stage2 加速。
- 端到端总查询未稳定达到 2%：
  - Amazon：-1.06% 到 +0.35%；
  - MSMARCO：最好 +1.09%，其余点略慢。
- conservative progressive pruning 没有提前 SafeOut，反而因为重复计算低 bit 变慢。
- 增量 progressive 在 Amazon `scale=0` 下有小幅端到端正收益，但 MSMARCO 未跑赢 fixed，且 Stage2 仍慢；因此不进入最终主方案。
- bound 放松后没有找到同时满足“两数据集 recall 下降 5% 内”和“查询速度优于 fixed active=3”的点。
- oracle 上限显示即使 bound 完美，MSMARCO 的 Stage2 内 progressive 剪枝空间也过小。
- 已清理失败候选索引 `amazon_esci/vector_bitmajor_tiles_ex3`，释放约 12G。
- 当前保留索引：
  - Amazon ESCI：`tile_lane_bitmajor_ex3`, `vector_bitplanes_ex3`
  - MSMARCO Passage：`tile_lane_bitmajor_ex3`, `vector_bitplanes_ex3`

因此，当前更稳妥的论文表述是：

- 新格式降低 Stage2 量化码扫描成本；
- 新格式支持 `stored_ex_bits=3` 的索引在查询时动态选择较低 `active_ex_bits` 作为速度/精度折中；
- 暂不声称其已经带来稳定 2% 以上端到端加速。

## 术语

- `stored_ex_bits`：磁盘和 resident Stage2 code 中物理存在的 bit-plane 数。
- `active_ex_bits`：当前查询要实际参与 dot-product 的 bit-plane 数，必须满足 `0 <= active_ex_bits <= stored_ex_bits`。
- `full Stage2`：`active_ex_bits == stored_ex_bits`。
- `partial Stage2`：`active_ex_bits < stored_ex_bits`。
- `dynamic Stage2 pruning`：在 Stage2 内部分步计算 bit-plane，并在每一步根据当前估计与 frontier 判断是否继续计算剩余 bit-plane。
