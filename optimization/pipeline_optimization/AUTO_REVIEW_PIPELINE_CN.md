# Pipeline Optimization Auto Review

时间：2026-06-25T15:17:36+08:00

## 目标

本轮 auto-review-loop 围绕在线查询 pipeline 的剩余瓶颈做多轮优化，实验数据统一写入：

`/home/zcq/VDB/test/pipeline_optimization_20260625`

统一基线口径：

- 复用已有 `cluster.clu` / `data.dat`，不重建索引，除非某轮优化必须改变索引格式。
- 默认开启 two-level coarse routing。
- 默认 `two_level_coarse_budget_factor = 16`。
- 默认 `topk=100`、`nprobe=256`、`non_safeout_candidate_budget=400`。
- 默认比较 compact resident 与 code-only 2MB slab HugePage。

## 当前瓶颈

根据 `/home/zcq/VDB/test/hugepage_codeslab_twolevel16_20260625` 的结果：

| 数据集 | 总耗时 | coarse | probe | Stage1 | Stage2 | submit/I/O | unaccounted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| amazon_esci | ~2.0 ms | ~18% | ~65-67% | ~22% | ~10% | ~18-19% | ~5-6% |
| msmarco_passage | ~3.4-3.5 ms | ~16-17% | ~68% | ~34% | ~8% | ~16-17% | ~9% |

结论：

1. two-level coarse routing 已经降低了 coarse score 的成本，但 topN / 候选整理仍有优化空间。
2. 最大瓶颈转移到 `probe`，其中 MSMARCO 的 Stage1 scan 最重。
3. submit/I/O pipeline 仍稳定占 `16-19%`，需要参考 LAANN 的“及时 I/O + 等待期间执行有用 CPU 工作”策略重新设计。

## 多轮路线

### Round 1: two-level warmup 前移

目标：

- 将 `PrepareTwoLevelCoarseRouting(nprobe)` 从查询计时路径前移到 preload/warmup。
- `avg_coarse_hierarchy_build_ms` 应接近 `0`。
- 新增 `two_level_coarse_warmup_ms`，单独记录 warmup 成本。

代码状态：

- `bench_e2e.cpp` 已经在 query round 前调用 warmup。
- 本轮补齐 `bench_online_query.cpp`。

实验目录：

`/home/zcq/VDB/test/pipeline_optimization_20260625/round1_twolevel_warmup`

### Round 2-3: Stage1 build/preload-time block envelope

目标：

- 不再采用此前负优化的 runtime envelope。
- 只做 build/preload-time 的 block summary / envelope。
- 不尝试 adaptive nprobe 或 cluster-level early stop。

验收指标：

- recall 不变。
- Stage1 ms 降低，尤其是 MSMARCO。
- 额外 resident memory 和 preload 时间可解释、可控。

### Round 4-7: submit / I/O pipeline

目标：

- 先基于 LAANN 方法写出 I/O 优化方案。
- 再做 3-4 轮小步优化，每轮保留独立结果。

候选方向：

- 高优先级候选优先 submit。
- I/O 等待期间继续处理低优先级但可能有用的候选。
- 批量 submit 时按地址局部性重排。
- 对已经不可能进入最终 top-k 的 pending read 做延迟/取消/降级。
- 进一步拆分 submit、wait、collector finalize、assemble result 的 tail timing。

## 状态

- Round 1 代码已修改并通过 `cmake --build build --target bench_e2e -j4`。
- Round 1 full sweep 已完成，结果见 `ROUND1_TWOLEVEL_WARMUP_CN.md`。
- Round 2 Stage1 build/preload-time block envelope 已完成，结果见 `ROUND2_STAGE1_PRECOMPUTE_ENVELOPE_CN.md`。
- Round 3-5 submit / I/O pipeline 已完成，结果见：
  - `ROUND3_IO_ADDRESS_SORT_CN.md`
  - `ROUND4_SUBMIT_CPU_CN.md`
  - `ROUND5_BUDGETED_EARLY_SUBMIT_CN.md`
  - `FINAL_PIPELINE_OPTIMIZATION_SUMMARY_CN.md`

## Round 1 评审

### Assessment

- Score: local review only，本轮没有可用外部 Codex MCP reviewer。
- Verdict: Round 1 sufficient。

### Criticisms

1. Warmup 前移只修正计时口径和真实在线服务初始化路径，不减少索引构建总成本。
2. 该改动让 coarse 占比下降，但也暴露 probe/Stage1 是更硬的瓶颈。
3. 后续 Stage1 envelope 不能重复此前 runtime envelope 的负优化路径，必须把可预计算部分移到 build/preload。

### Actions Taken

- `bench_online_query.cpp` 在创建 scheduler 前调用 `SetTwoLevelCoarseRouting` 和 `PrepareTwoLevelCoarseRouting`。
- 新增 `two_level_coarse_warmup_ms` JSON 指标。
- 复用已有索引完成 36 个结果点。

### Results

- `avg_coarse_hierarchy_build_ms = 0`。
- ESCI 平均查询延迟相对上一轮下降约 `9.4-10.8%`。
- MSMARCO 平均查询延迟相对上一轮下降约 `6.7-10.5%`。
- recall 不变。

## Round 2 评审

### Assessment

- Score: local review only，本轮没有可用外部 Codex MCP reviewer。
- Verdict: not adopted。

### Criticisms

1. 预计算 summary 没有解决核心问题：query-time block envelope 仍需要按 group 查 LUT 求上界。
2. block skip 命中率过低，不具备成为主路径优化的条件。
3. 额外 resident memory 虽然可控，但在负优化时没有保留价值。

### Actions Taken

- 实现 `VDB_STAGE1_PRECOMPUTE_ENVELOPE=1` 的 preload-time summary。
- 完成 ESCI/MSMARCO `total_bits=4/ex_bits=3` 最小 sweep。

### Results

- ESCI code-slab HP：`1.7881 ms -> 7.7581 ms`，明显变慢。
- MSMARCO code-slab HP：`3.1081 ms -> 18.6249 ms`，明显变慢。
- skip rate 仅 `0.28-0.47%`。
- recall 不变。

### Decision

Stage1 block envelope 路线停止，保留为诊断开关。后续进入 LAANN-informed submit/I/O pipeline 优化。

## Round 3-5 评审

### Assessment

- Score: local review only，本轮没有可用外部 Codex MCP reviewer。
- Verdict: I/O pipeline rounds completed; only warmup and submit CPU cleanup adopted。

### Actions Taken

- Round 3：实现 vec-only read address sort，可选开关默认关闭。
- Round 4：去掉 budget materialization 重复排序，预留 pending plan 容量。
- Round 5：实现 budgeted early-submit，可选开关默认关闭。

### Results

- address sort：负优化。
- submit CPU cleanup：基本持平。
- budgeted early-submit：降低 submit/drain，但总耗时变慢。

### Final Decision

默认路径只采用：

1. two-level hierarchy warmup 前移；
2. submit CPU 低风险清理。

其他三项保留为实验开关，不进入主结果。
