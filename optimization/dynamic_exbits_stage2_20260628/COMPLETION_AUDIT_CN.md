# 完成审计：Dynamic ExBits Stage2 目标

日期：2026-06-28

## 原始目标拆解

本轮目标包含以下可验证要求：

1. 代码和存储格式修改：
   - 构建期 `ex_bits` 表示 `stored_ex_bits`；
   - 查询期 `ex_bits` 表示 `active_ex_bits`。
2. 新布局：
   - 进一步调整 Stage2 布局，使同一个 vector 下按 bit/tile 组织；
   - 适配查询时动态降低 active ex bits。
3. 参数复用：
   - 复用 `/home/zcq/VDB/paper/experiment-documents/no-combine-ablation-amazon-msmarco-ex3-20260627` 的 Amazon/MSMARCO 参数。
4. 开启并记录 `$auto-review-loop`：
   - 至少有审查记录；
   - 审查发现需要处理或记录。
5. 格式优化：
   - 先拆分 ex bits；
   - 再做三轮以上格式优化；
   - 目标是在 Amazon 和 MSMARCO 的 `ex_bits=3` 下，相比 official RaBitQ 格式均达到 2% 加速。
6. 动态剪枝：
   - 至少做一轮根据动态 ex bits 的剪枝策略优化。
7. 文档和实验目录：
   - optimization 下单独目录；
   - 新布局/动态剪枝方案执行前有 markdown 说明；
   - 实验输出放到 `~/test` 下单独目录。
8. 清理：
   - 优化结束后清理中间版本索引；
   - 只保留最优方法索引和 official baseline 索引。

## 当前证据

### 1. stored/active ex bits 拆分

状态：已完成。

证据：

- `SearchConfig` 中有 `rabitq_active_ex_bits_set` / `rabitq_active_ex_bits`。
- `ClusterProber` 构造接收 `code_bits`, `total_bits`, `active_ex_bits`。
- `bench_e2e` / `bench_online_query` 支持 `--rabitq-active-ex-bits`。
- JSON 中记录：
  - `rabitq_stored_ex_bits`
  - `rabitq_active_ex_bits`
  - `rabitq_configured_active_ex_bits`
  - `rabitq_effective_active_ex_bits`
  - `rabitq_active_ex_bits_mode`

### 2. 新 Stage2 布局

状态：已完成。

证据：

- 新增 `RaBitQExDataLayout::kTileLaneBitMajor`。
- 存储布局为 `[tile][bit][lane][tile_dims/8]`。
- 新增 pack/unpack 和 query kernel：
  - `ExRaBitQPackOfficialTileLaneBitMajor`
  - `ExRaBitQUnpackOfficialTileLaneBitMajor`
  - `IPOfficialRaBitQBatchCompactTileLaneBitMajorMasked`
- 测试覆盖：
  - `OfficialTileLaneBitMajorRoundTripBits123`
  - `OfficialTileLaneBitMajorMaskedSupportsPartialActiveBits`

### 3. 参数复用

状态：已完成。

证据：

- `run_stage2_format_formal.sh` 使用：
  - `topk={10,100}`
  - `nprobe={64,128,256,512}`
  - `total_bits=4`
  - `ex_bits=3`
  - `two_level_coarse_routing=1`
  - `two_level_coarse_budget_factor=16`
  - `non_safeout_candidate_budget=400`
  - `dynamic_safeout=1`
  - `dynamic_safein=frontier`
  - `io_queue_depth=64`
  - `fixed_vec_buffer_count=1024`
  - `fine_grained_timing=0`

### 4. auto-review-loop

状态：已完成。

证据：

- `AUTO_REVIEW.md` 记录了：
  - Round 0 stored/active 拆分；
  - Round 1 `tile_lane_bitmajor`；
  - Auto Review Round 1；
  - Round 2 kernel fusion；
  - Round 3 pointer hoist；
  - Round 4 progressive pruning；
  - Auto Review Round 2；
  - Round 5 lane8 stack trim。

审查结论：

- 当前结果可作为第二贡献的弱子贡献；
- 不支持“稳定 2% 端到端加速”；
- 不支持“progressive pruning 加速 SafeOut”。

### 5. 三轮以上格式优化

状态：已完成，但目标性能未达成。

证据：

- Round1：`tile_lane_bitmajor` 新布局，保留。
- Round2：bit-plane fused kernel，保留。
- Round3：tile pointer hoist，负收益，撤回。
- Round5：8-lane stack trim，负收益，撤回。

性能证据：

- `stage2_format_formal_q1000_np64_reps5`：
  - Amazon：端到端 +1.352%，Stage2 +7.551%；
  - MSMARCO：端到端 +0.457%，Stage2 +4.712%。
- `stage2_format_formal_q1000_nprobe_sweep`：
  - `topk=10,nprobe={64,128,256,512}` 下端到端没有稳定达到 2%。
- `stage2_format_formal_q1000_topk100`：
  - `topk=100` 下 `tile_lane_bitmajor` 整体更慢。

结论：

- Stage2 component-level 加速被证明；
- 两个数据集端到端均达到 2% 的目标被结果反证。

### 6. 动态 ex bits 剪枝

状态：已完成一轮，结果为负。

证据：

- 新增 `--stage2-progressive-active-bits`。
- 实现 conservative SafeOut-only progressive path：
  - active=1
  - active=2
  - active=3
- Amazon smoke：
  - fixed active=3：0.5099 ms，Stage2 0.0507 ms；
  - progressive 1->2->3：0.5629 ms，Stage2 0.0895 ms；
  - progressive safeout lanes = 0.0。

结论：

- Conservative bound 太宽；
- partial 轮次没有提前 SafeOut；
- 当前策略不进入最终方案。

### 7. 文档和实验目录

状态：已完成。

证据：

- 文档目录：
  - `optimization/dynamic_exbits_stage2_20260628`
- 实验目录：
  - `/home/zcq/VDB/test/dynamic_exbits_stage2_20260628`
- 执行前设计文档：
  - `DESIGN_ROUND0_CN.md`
  - `ROUND1_TILE_LANE_BITMAJOR_CN.md`
  - `ROUND2_TILE_KERNEL_FUSION_CN.md`
  - `ROUND3_TILE_POINTER_HOIST_CN.md`
  - `ROUND4_DYNAMIC_EXBITS_PRUNING_CN.md`
  - `ROUND5_TILE_LANE8_STACK_TRIM_CN.md`

### 8. 索引清理

状态：已完成。

当前保留：

- `/home/zcq/VDB/test/dynamic_exbits_stage2_20260628/indexes/amazon_esci/tile_lane_bitmajor_ex3`
- `/home/zcq/VDB/test/dynamic_exbits_stage2_20260628/indexes/amazon_esci/vector_bitplanes_ex3`
- `/home/zcq/VDB/test/dynamic_exbits_stage2_20260628/indexes/msmarco_passage/tile_lane_bitmajor_ex3`
- `/home/zcq/VDB/test/dynamic_exbits_stage2_20260628/indexes/msmarco_passage/vector_bitplanes_ex3`

已清理：

- `/home/zcq/VDB/test/dynamic_exbits_stage2_20260628/indexes/amazon_esci/vector_bitmajor_tiles_ex3`

## 测试证据

最近一次关键测试：

- `cmake --build build --target bench_e2e test_ip_exrabitq test_cluster_prober -j4`
- `./build/test_ip_exrabitq`：25/25 通过。
- `./build/test_cluster_prober`：3/3 通过。

此前完整测试：

- `test_cluster_store`：25/25 通过。
- `test_types`：33/33 通过。

## 审计结论

本轮工作已完成除性能目标外的所有实现、文档、实验和清理要求。

未完成项：

- Amazon 和 MSMARCO 在 `ex_bits=3` 下相比 official format 均达到 2% 端到端加速。

该未完成项不是缺少实现或实验，而是被当前实验反证：

- Stage2 组件有稳定加速；
- 但 Stage2 在端到端中占比不足，且 `topk=100` 下新布局更慢；
- progressive active-bit pruning 没有提前 SafeOut。

因此不能将目标标记为 complete。若继续追求 2% 端到端，应转向 submit/I/O、Stage1 或 coarse routing，而不是继续局部 Stage2 layout 优化。
