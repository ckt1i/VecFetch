# Code-Only 2MB Slab HugePage 两轮优化结果

时间：2026-06-25

## 背景

此前 `full_file_mmap_2mb` 在 ESCI 和 MSMARCO `total_bits=2/3` 上有效，但在 MSMARCO `total_bits=4/ex_bits=3` 上出现倒退。已有 timing 表明，倒退不是来自 RaBitQ 主扫描：

- `coarse`、`probe`、`Stage1`、`Stage2` 多数变快。
- 总延迟倒退主要来自 Search tail，尤其 `final_drain`。

因此本轮目标是把 2MB hugepage 作用范围从整个 `cluster.clu` 缩小到真正热的 RaBitQ code region。

## 实现

新增 resident preload 模式：

```bash
VDB_RESIDENT_PRELOAD_MODE=compact_code_mmap_2mb
VDB_RESIDENT_HUGEPAGE=1
VDB_RESIDENT_HUGEPAGE_COLLAPSE=1
```

该模式在 compact resident preload 下：

1. 为所有 cluster 的 RaBitQ code region 分配一个 2MB 对齐的 anonymous mmap slab。
2. 只把 `fastscan + exdata` code region 拷贝进 slab。
3. decoded address、metadata、parallel Stage2 view 仍按原 compact resident 方式管理。
4. slab 写满后调用 `madvise(MADV_HUGEPAGE)`，设置 collapse 时再调用 `MADV_COLLAPSE`。

新增 tail timing 字段：

- `avg_final_drain_ms`
- `avg_execute_buffered_ms`
- `avg_collector_finalize_ms`
- `avg_assemble_results_ms`
- `avg_search_unaccounted_ms`

## Round 1：Code-Only Slab 与 Tail Timing

设置：

- 数据集：`msmarco_passage`
- 索引：`current_index_official_1_plus_n_total4_ex3_vector_bitmajor_tiles`
- `topk=100`
- `nprobe=256`
- `queries=1000`
- 每点 3 次

结果：

| 模式 | avg ms | recall@100 | preload ms | preload bytes GB | resident mem GB | AnonHugePages GB | Stage1 ms | Stage2 ms | final_drain ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| compact default | 4.1791 | 0.92443 | 1683.56 | 3.388 | 3.425 | 0.000 | 1.2606 | 0.2866 | 0.1267 |
| full-file mmap2mb + collapse | 4.6064 | 0.92443 | 2153.32 | 3.438 | 3.569 | 3.438 | 1.1485 | 0.2736 | 0.6945 |
| code-only slab mmap2mb + collapse | 3.9570 | 0.92443 | 2614.45 | 6.776 | 3.425 | 3.293 | 1.1401 | 0.2705 | 0.1257 |

结论：

- full-file 模式的 `final_drain` 从 `0.1267ms` 增加到 `0.6945ms`，解释了 MSMARCO bits=4 的总延迟倒退。
- code-only slab 的 `final_drain` 与 compact 基本一致，同时 Stage1/Stage2 仍然受益。
- Round 1 code-only slab 的主要问题是 preload：为了计算 dense slab offset，第一版 prepass 读了一遍完整 cluster blocks，使 `preload_bytes` 翻倍。

## Round 2：Trailer-Only Prepass

Round 2 将 code-slab prepass 改为只读每个 cluster block 尾部 mini-trailer：

- 读取 footer 得到 `mini_trailer_size` 与 magic。
- 读取 mini-trailer，校验 address metadata/footer。
- 通过 `region1_size + region2_size` 计算 `code_region_bytes`。
- 主 pass 仍批量读取完整 blocks 并填入 dense slab。

同一设置结果：

| 模式 | avg ms | recall@100 | preload ms | preload bytes GB | resident mem GB | AnonHugePages GB | Stage1 ms | Stage2 ms | final_drain ms |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| compact default | 4.2101 | 0.92443 | 1675.54 | 3.388 | 3.425 | 0.000 | 1.2607 | 0.2869 | 0.1261 |
| full-file mmap2mb + collapse | 4.5862 | 0.92443 | 2160.47 | 3.438 | 3.569 | 3.438 | 1.1439 | 0.2733 | 0.7080 |
| code-only slab mmap2mb + collapse | 4.0113 | 0.92443 | 2409.68 | 3.389 | 3.425 | 3.293 | 1.1559 | 0.2736 | 0.1233 |

Round 2 相对 Round 1 code-only slab：

| 指标 | Round 1 | Round 2 | 变化 |
| --- | ---: | ---: | ---: |
| preload bytes GB | 6.776 | 3.389 | -50.0% |
| preload ms | 2614.45 | 2409.68 | -204.78ms |
| avg query ms | 3.9570 | 4.0113 | +0.0544ms |
| recall@100 | 0.92443 | 0.92443 | 0 |

Round 2 code-only slab 相比同轮 compact：

- 查询加速：`4.2101 / 4.0113 = 1.0496x`
- recall delta：0
- `AnonHugePages`：约 `3.293GB`
- `resident_cluster_mem_bytes`：与 compact 基本一致，避免了 full-file 模式额外常驻冷数据。

## 结论

1. `full_file_mmap_2mb` 不应作为默认策略。它虽然让 Stage1/Stage2 更快，但 MSMARCO bits=4 的 `final_drain` 明显倒退。
2. `compact_code_mmap_2mb` 是当前更合理的 hugepage 方案。它只对 RaBitQ code hot region 使用 2MB slab，能避免 full-file 冷数据污染。
3. trailer-only prepass 已把 code-slab preload I/O 从 6.78GB 降到 3.39GB，但 preload 时间仍高于 compact。
4. 若要继续优化，最有效的方向是在构建期把每 cluster 的 `code_region_bytes` 或 code offset 写入 metadata，从而查询时不再需要任何 prepass。

## 推荐策略

- 默认仍使用 `compact_batched`。
- 当部署环境确认 THP 可 materialize，且 workload 类似 MSMARCO bits=4 的查询热路径时，可启用：

```bash
VDB_RESIDENT_PRELOAD_MODE=compact_code_mmap_2mb
VDB_RESIDENT_HUGEPAGE=1
VDB_RESIDENT_HUGEPAGE_COLLAPSE=1
```

- 不建议默认启用 `full_file_mmap_2mb`，除非数据集/bit sweep 证明不存在 final drain 倒退。

## 结果路径

- Round 1: `/home/zcq/VDB/test/hugepage_autoreview_20260625/runs/round1/msmarco_passage/bits4/topk100_np256/`
- Round 2: `/home/zcq/VDB/test/hugepage_autoreview_20260625/runs/round2/msmarco_passage/bits4/topk100_np256/`
- Round 1 runner: `/home/zcq/VDB/test/hugepage_autoreview_20260625/run_round1_msmarco_bits4.sh`

## 验证

已通过：

```bash
cmake --build build --target bench_e2e bench_build_index test_cluster_prober -j4
./build/test_cluster_prober
cmake --build build --target test_cluster_store test_segment test_overlap_scheduler -j4
./build/test_cluster_store
./build/test_segment
./build/test_overlap_scheduler
git diff --check
```

测试结果：

- `test_cluster_prober`: 3 tests passed
- `test_cluster_store`: 25 tests passed
- `test_segment`: 8 tests passed
- `test_overlap_scheduler`: 20 tests passed
- `git diff --check`: passed
