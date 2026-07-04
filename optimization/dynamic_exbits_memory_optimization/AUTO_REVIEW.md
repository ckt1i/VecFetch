# Auto Review Loop - RaBitQ Stage1 / HugePage / Format Sweep

时间：2026-06-24T23:21:05+08:00

## 目标

本轮 auto-review-loop 围绕三个问题推进：

1. 在 `amazon_esci`、`ex_bits=3` 下实现并验证 Stage1 block-level skip envelope。
2. 分析 Linux HugePage 是否能优化 resident RaBitQ 内存扫描，写出方案并做可行性验证。
3. 在 `amazon_esci` 与 `msmarco_passage` 上完成 `ex_bits=1,2,3` sweep，对比当前 `new_vector_bitmajor_tiles` 与此前 `official_vector_bitplanes` 格式。

## Round 1: Stage1 Block Skip Envelope

实现内容：

- 在 `ClusterProber::Probe()` 中增加可选 `enable_stage1_block_skip_envelope`。
- 对每个 FastScan block 计算保守 SafeOut 下界：
  - 每个 group 取 block 内出现过 nibble 的最大 LUT 贡献。
  - 转换为 `ip_upper`。
  - 使用 block 内 `norm_min/norm_max` 在区间上最小化 `dist_lower - margin`。
  - 仅当该下界严格大于 `safeout_frontier_upper` 时整 block 跳过。
- CLI 增加 `--stage1-block-skip-envelope`，并记录 envelope 测试/跳过 block 数。
- 增加 `ClusterProberTest.Stage1BlockSkipEnvelopeSkipsSafeOutTailBlock` 覆盖 tail block。

ESCI `ex_bits=3`, `topk=100`, `nprobe=256`, 1000 queries 结果：

| 设置 | avg ms | recall@100 | avg Stage1 ms | tested blocks | skipped blocks | skipped lanes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| off | 1.7987 | 0.9217 | 0.4663 | 0.0 | 0.0 | 0.0 |
| on | 10.8619 | 0.9217 | 8.5892 | 2008.564 | 9.444 | 14.989 |

结论：

- 判定逻辑是安全的，recall 不变。
- 运行时计算 envelope 的成本远大于收益，skip 命中率过低。
- 该优化应保留为诊断开关，默认关闭；不作为当前主结果优化。

结果路径：

- `/home/zcq/VDB/test/rabitq_fair_ex3_20260624/runs/stage1_envelope_runtime/main1000/amazon_esci/new_vector_bitmajor_tiles/topk100_np256/off/amazon_esci_20260624T182205/results.json`
- `/home/zcq/VDB/test/rabitq_fair_ex3_20260624/runs/stage1_envelope_runtime/main1000/amazon_esci/new_vector_bitmajor_tiles/topk100_np256/on/amazon_esci_20260624T182214/results.json`

## Round 2: HugePage Feasibility

实现内容：

- 增加 `VDB_RESIDENT_HUGEPAGE=1`，对 resident buffers 调用 `madvise(MADV_HUGEPAGE)`。
- 增加 `VDB_RESIDENT_HUGEPAGE_COLLAPSE=1`，在支持时尝试 `MADV_COLLAPSE`。
- 在 `bench_e2e` 输出 `smaps_anon_huge_pages_kib` 与 `smaps_file_pmd_mapped_kib`。
- 写入方案文档：`rabitq_code_zip/HUGEPAGE_RABITQ_OPTIMIZATION_PLAN_CN.md`。

ESCI `ex_bits=3`, `topk=100`, `nprobe=256`, 1000 queries 结果：

| 模式 | avg ms | recall@100 | Stage1 ms | Stage2 ms | AnonHugePages KiB | FilePmdMapped KiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| compact, HP off | 1.8471 | 0.9217 | 0.4873 | 0.1891 | 0.0 | 0.0 |
| compact, HP on | 1.8376 | 0.9217 | 0.4779 | 0.1888 | 0.0 | 0.0 |
| full_file, HP off | 1.8242 | 0.9217 | 0.4810 | 0.1893 | 0.0 | 0.0 |
| full_file, HP on | 1.8338 | 0.9217 | 0.4808 | 0.1897 | 0.0 | 0.0 |
| full_file, HP on + collapse | 1.7938 | 0.9217 | 0.4458 | 0.1819 | 0.0 | 0.0 |

结论：

- 当前环境 THP 没有实际 materialize，`AnonHugePages` 和 `FilePmdMapped` 均为 0。
- `MADV_COLLAPSE` 有单次轻微收益，但没有 huge page 证据，不能作为稳定优化结论。
- HugePage 保留为可选诊断/部署探索项，不进入主实验优化路径。

结果路径：

- `/home/zcq/VDB/test/rabitq_fair_ex3_20260624/runs/hugepage_probe/amazon_esci/new_vector_bitmajor_tiles/topk100_np256/`

## Full Sweep: ESCI / MSMARCO ex_bits=1,2,3

查询设置：

- `dynamic-safeout=1`
- `dynamic-safein=frontier`
- `non-safeout-candidate-budget=400`
- `queries=1000`
- `stage1-block-skip-envelope=0`
- `fine-grained-timing=0`

完整结果：

- 原始表：`/home/zcq/VDB/test/rabitq_autoreview_20260624/results/format_sweep_summary.csv`
- 配对加速比：`/home/zcq/VDB/test/rabitq_autoreview_20260624/results/format_sweep_paired_speedup.csv`
- Markdown 摘要：`/home/zcq/VDB/test/rabitq_autoreview_20260624/results/format_sweep_summary.md`

结果覆盖：

- `amazon_esci`: 2 variants x 3 total_bits x 2 topk x 4 nprobe = 48 results。
- `msmarco_passage`: 2 variants x 3 total_bits x 2 topk x 3 nprobe = 36 results。
- 总计 84 results, 42 paired comparisons。

平均 speedup，定义为 `official_vector_bitplanes avg_ms / new_vector_bitmajor_tiles avg_ms`：

| 数据集 | total_bits | paired points | mean speedup | min | max |
| --- | ---: | ---: | ---: | ---: | ---: |
| amazon_esci | 2 | 8 | 0.9974 | 0.9829 | 1.0099 |
| amazon_esci | 3 | 8 | 0.9903 | 0.9644 | 1.0172 |
| amazon_esci | 4 | 8 | 1.0174 | 0.9862 | 1.0504 |
| msmarco_passage | 2 | 6 | 1.1159 | 0.9563 | 1.5515 |
| msmarco_passage | 3 | 6 | 1.1121 | 0.9562 | 1.4683 |
| msmarco_passage | 4 | 6 | 1.0142 | 0.9981 | 1.0515 |

整体结论：

- `amazon_esci` 上新格式总体接近持平，`total_bits=4` 有小幅收益。
- `msmarco_passage` 上新格式平均有收益，尤其 `total_bits=2/3` 的低 nprobe 点收益较明显，但高 nprobe 仍有持平或轻微倒退点。
- 所有 paired comparison 的 recall delta 均为 0，说明格式对比没有改变搜索结果。

## Verification

已通过：

```bash
cmake --build build --target bench_e2e bench_build_index test_cluster_prober -j4
./build/test_cluster_prober
```

`test_cluster_prober` 结果：3 tests passed。

## Final Assessment

- Stage1 block-level runtime envelope：安全但负优化，默认关闭。
- Linux HugePage：代码可选支持已加入，当前环境未产生 huge pages，不作为主优化。
- 当前格式优化：完整 sweep 已完成；新格式在 ESCI 基本持平，在 MSMARCO 有平均收益但不是单调全胜。

## Follow-up: 2MB Aligned mmap HugePage Probe

时间：2026-06-25

根据后续要求，本轮进一步实现了显式 2MB 对齐大块 resident 分配：

- 新增 `VDB_RESIDENT_PRELOAD_MODE=full_file_mmap_2mb`。
- full-file resident 使用匿名 `mmap()` 分配 `cluster.clu` 大块 buffer，并将数据起始地址对齐到 2MB。
- 可叠加 `VDB_RESIDENT_HUGEPAGE=1` 与 `VDB_RESIDENT_HUGEPAGE_COLLAPSE=1`。
- 复用已有索引，不重建 `cluster.clu`/`data.dat`。
- `bench_online_query` 增加 `smaps_rollup` hugepage 字段。

单数据集验证：

- ESCI `ex_bits=3` 完整 benchmark，8 次 paired 验证：
  - compact: 1.7471 ms
  - mmap2mb + HP + collapse: 1.6813 ms
  - paired 平均降幅：3.76%
  - recall delta: 0
  - full benchmark 下 `AnonHugePages=0`，因此不能单独证明 THP materialize。

扩展 online-query 验证：

| 数据集 | total_bits | speedup |
| --- | ---: | ---: |
| amazon_esci | 2 | 1.0339 |
| amazon_esci | 3 | 1.0396 |
| amazon_esci | 4 | 1.0194 |
| msmarco_passage | 2 | 1.0783 |
| msmarco_passage | 3 | 1.0748 |
| msmarco_passage | 4 | 0.9230 |

online-query 下 `AnonHugePages` 明确非零，说明 2MB 对齐 mmap 路径可以让 THP 实际 materialize。但 MSMARCO bits=4 总延迟倒退，因此该优化应保留为可选系统优化，不应无条件替代默认 compact resident。

详细记录：

- `rabitq_code_zip/HUGEPAGE_MMAP2MB_RESULTS_CN.md`
- `/home/zcq/VDB/test/hugepage_mmap2mb_20260625/results/esci_ex3_mmap2mb_summary.md`
- `/home/zcq/VDB/test/hugepage_mmap2mb_20260625/results/online_extension_summary.md`

## Follow-up: Two-Round HugePage Auto Review

时间：2026-06-25

目标：

- Round 1：针对 MSMARCO `total_bits=4/ex_bits=3` 的 full-file 2MB mmap 倒退，实现更细粒度的 code-only 2MB slab packing，并补充 Search tail timing。
- Round 2：根据 Round 1 的用时定位，优化 code-slab 的构建/预加载成本。

### Round 1: Code-Only 2MB Slab 与 Tail Timing

实现内容：

- 新增 resident preload 模式：

```bash
VDB_RESIDENT_PRELOAD_MODE=compact_code_mmap_2mb
VDB_RESIDENT_HUGEPAGE=1
VDB_RESIDENT_HUGEPAGE_COLLAPSE=1
```

- compact preload 下仅将每个 cluster 的 RaBitQ code region 拷贝到一个 2MB 对齐的 dense anonymous mmap slab。
- metadata、decoded address、parallel Stage2 view 仍保持原 compact resident 结构，避免 full-file mmap 把 `cluster.clu` 冷数据一起常驻。
- `SearchStats` 和 benchmark JSON 增加 `avg_final_drain_ms`、`avg_execute_buffered_ms`、`avg_collector_finalize_ms`、`avg_assemble_results_ms`、`avg_search_unaccounted_ms`。

MSMARCO `total_bits=4/ex_bits=3`, `topk=100`, `nprobe=256`, `queries=1000`, 每点 3 次：

| 模式 | avg ms | recall@100 | preload ms | resident mem GB | AnonHugePages GB | final_drain ms | unaccounted ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| compact default | 4.1791 | 0.92443 | 1683.56 | 3.425 | 0.000 | 0.1267 | 0.3939 |
| full-file mmap2mb + collapse | 4.6064 | 0.92443 | 2153.32 | 3.569 | 3.438 | 0.6945 | 0.3478 |
| code-only slab mmap2mb + collapse | 3.9570 | 0.92443 | 2614.45 | 3.425 | 3.293 | 0.1257 | 0.3520 |

结论：

- full-file mmap2mb 的倒退不是 Stage1/Stage2：其 Stage1/Stage2 仍然更快，但 `final_drain_ms` 从约 `0.127ms` 增加到约 `0.695ms`，这是 MSMARCO bits=4 总延迟倒退的主因。
- code-only slab 避免了 full-file 冷数据常驻，`final_drain_ms` 回到 compact 水平，同时保留了 2MB hugepage 对 Stage1/Stage2 的收益。
- Round 1 code-slab 的问题是 preload 读量翻倍：为了计算 dense slab offset，prepass 读了一遍完整 cluster blocks，导致 `preload_bytes` 约 6.78GB，preload 时间约 2.61s。

### Round 2: Trailer-Only Code-Slab Prepass

实现内容：

- 将 code-slab prepass 从“批量读完整 cluster block 并解析”改为“只读每个 block 尾部 mini-trailer”。
- 通过 trailer 中的 `v9_payload_offset`、address payload metadata 和 footer 校验计算 `code_region_bytes`。
- 主 pass 仍保持 16-block batch read，并将 code region 填入 dense 2MB slab。

同一 MSMARCO 设置，每点 3 次：

| 模式 | avg ms | recall@100 | preload ms | preload bytes GB | resident mem GB | AnonHugePages GB | final_drain ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| compact default | 4.2101 | 0.92443 | 1675.54 | 3.388 | 3.425 | 0.000 | 0.1261 |
| full-file mmap2mb + collapse | 4.5862 | 0.92443 | 2160.47 | 3.438 | 3.569 | 3.438 | 0.7080 |
| code-only slab mmap2mb + collapse | 4.0113 | 0.92443 | 2409.68 | 3.389 | 3.425 | 3.293 | 0.1233 |

Round 2 相比 Round 1 的 code-slab：

- preload bytes：`6.776GB -> 3.389GB`，约减半。
- preload time：`2614.45ms -> 2409.68ms`，降低约 `204.8ms`。
- query avg：`3.9570ms -> 4.0113ms`，有约 `1.4%` 波动，但仍比同轮 compact 快约 `4.7%`。
- recall 保持完全一致。

最终结论：

- `full_file_mmap_2mb` 不适合作为默认策略，MSMARCO bits=4 会因 Search tail / final drain 倒退。
- `compact_code_mmap_2mb` 是更合理的 hugepage 路径：只对 RaBitQ code hot region 做 2MB 对齐与 collapse，保留 query speedup，同时避免 full-file 冷数据污染。
- trailer-only prepass 已显著降低 code-slab preload I/O，但 preload 时间仍高于 compact；若未来要默认启用，应继续优化为构建期记录每 cluster code length/offset，彻底去掉查询时 prepass。

结果路径：

- Round 1: `/home/zcq/VDB/test/hugepage_autoreview_20260625/runs/round1/msmarco_passage/bits4/topk100_np256/`
- Round 2: `/home/zcq/VDB/test/hugepage_autoreview_20260625/runs/round2/msmarco_passage/bits4/topk100_np256/`
- 结果说明：`rabitq_code_zip/HUGEPAGE_CODE_SLAB_RESULTS_CN.md`

验证：

```bash
cmake --build build --target bench_e2e bench_build_index test_cluster_prober -j4
./build/test_cluster_prober
cmake --build build --target test_cluster_store test_segment test_overlap_scheduler -j4
./build/test_cluster_store
./build/test_segment
./build/test_overlap_scheduler
git diff --check
```

结果：`test_cluster_prober` 3 tests passed，`test_cluster_store` 25 tests passed，`test_segment` 8 tests passed，`test_overlap_scheduler` 20 tests passed，`git diff --check` 通过。

## Follow-up: Selective Preload Claim Review

时间：2026-06-30T16:59:18+08:00

背景：

- 冻结方案采用当前 RaBitQ 存储格式与 fixed 查询方案。
- 新增强 claim 方向：一个最大 bits 的物理索引可以在启动时按照内存预算选择较低 bits resident preload，而不是为每个 bits 单独构建索引。
- 本轮 Codex MCP 外部审稿工具不可用，因此采用本地 self-review，并通过最小实验验证关键可行性。

实现内容：

- `bench_e2e` 增加用户可见参数：
  - `--rabitq-active-bits N`
  - `--rabitq-resident-bits N`
  - 旧脚本兼容逻辑仅保留在代码内部，不再作为用户可见参数表述。
- `ClusterStoreReader::PreloadAllClusters()` 支持 selective resident load。
- 对 `tile_lane_bitmajor` / `vector_bitmajor_tiles` 这类 bit-prefix 友好布局，在 resident preload 时只拷贝所需前缀 bitplanes，并保留原始物理索引中的 factor metadata。
- 查询侧区分 stored bits、active bits 与 resident-loaded bits，避免用低 resident bits 的内存布局误按满 bits stride 访问。

Amazon ESCI 最小实验设置：

- 复用现有 bits=4 物理索引：
  `/home/zcq/VDB/test/dynamic_exbits_stage2_20260628/indexes/amazon_esci/tile_lane_bitmajor_ex3/current_index_official_1_plus_n_total4_ex3_tile_lane_bitmajor`
- `query-count=1000`，`topk=10`，`nprobe=256`
- two-level coarse routing 开启，`budget_factor=16`
- fixed 查询方案，关闭 progressive
- 结果目录：
  `/home/zcq/VDB/test/selective_preload_amazon_esci_20260630_nprobe256/`

结果：

| 配置 | resident 模式 | code MB | cluster mem MB | preload ms | avg ms | QPS | recall@10 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| active bits=4, resident bits=4 | compact_batched | 699.4 | 727.1 | 377.9 | 1.4146 | 706.9 | 0.8917 |
| active bits=3, resident bits=4 | compact_batched | 699.4 | 727.1 | 378.2 | 1.3975 | 715.6 | 0.7842 |
| active bits=3, resident bits=3 | compact_selective_bits3 | 533.3 | 561.0 | 346.3 | 1.3790 | 725.2 | 0.7842 |
| active bits=2, resident bits=4 | compact_batched | 699.4 | 727.1 | 378.0 | 1.4034 | 712.6 | 0.7361 |
| active bits=2, resident bits=2 | compact_selective_bits2 | 367.1 | 394.8 | 314.3 | 1.3790 | 725.2 | 0.7361 |

评审结论：

- 当前存储格式足够支持 selective preload，但前提是采用 bit-prefix 友好的布局；`tile_lane_bitmajor` 可以直接按前缀 bitplanes 物化 resident code。
- selective preload 的强项是 resident memory，而不是查询速度：ESCI 上 resident bits=3 将 cluster resident memory 从 727.1 MB 降至 561.0 MB，约降低 22.8%；resident bits=2 降至 394.8 MB，约降低 45.7%。
- `nprobe=256` 后 bits=4 的 recall@10 提升到 0.8917，修正了 `nprobe=64` 下准确率偏低的问题。
- 查询时间在最小实验中不是稳定主收益；因此 paper 中不应 claim “格式相比 RaBitQ 官方布局带来查询加速”，应 claim “一个物理索引支持多运行精度/内存配置，降低部署和内存成本，且不改变结果口径”。
- 若采用强 claim，不能只在普通 bits-vs-baseline 表里顺带提到；至少需要一个小对比实验证明同一个 bits=4 物理索引在启动时选择 resident bits=2/3/4 的内存收益、recall 变化和查询开销。

建议写法：

- 将该点放入格式贡献的子 claim：physical max-bits index, runtime precision materialization。
- 主实验仍按 bits-vs-baseline 展示整体 Pareto。
- 追加一个小消融表，列出同物理索引下 `resident bits=4/3/2` 的 resident memory、preload time、QPS、recall@10；该表可以只做 Amazon ESCI 或 COCO+ESCI 两个数据集。

验证：

```bash
cmake --build build --target bench_e2e test_ip_exrabitq test_cluster_prober test_cluster_store -j 8
./build/test_ip_exrabitq
./build/test_cluster_prober
./build/test_cluster_store
```

结果：`test_ip_exrabitq` 26 tests passed，`test_cluster_prober` 3 tests passed，`test_cluster_store` 25 tests passed。

## Method Description

当前冻结方案将向量索引与原始数据访问视作一个联合系统：聚类索引不只返回 id，而是在索引记录中直接携带后续读取原始向量与 payload 所需的地址信息。在线查询时，系统先在 resident RaBitQ code 上完成近似筛选和 SafeIn/SafeOut 判定，再对不确定候选触发原始向量或原始数据读取；流水线调度负责把候选提交、磁盘读取和 rerank 计算重叠起来，从而把向量搜索与数据获取合并优化。

本轮 selective preload 进一步把格式贡献限定为“一个最大 bits 物理索引支持多运行精度”：构建时保存 bits=4 的 bit-prefix 友好 code layout，启动时可以根据内存预算只 resident materialize bits=3 或 bits=2 的前缀 bitplanes。查询侧仍以 total bits 作为用户可见配置，并在内部把 resident-loaded layout bits 与 stored bits 分开处理，避免低精度 resident view 按满精度 stride 访问。
