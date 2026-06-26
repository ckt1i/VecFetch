# Round 1: Two-Level Coarse Routing Warmup 前移

时间：2026-06-25T15:17:36+08:00

## 目的

此前 `bench_online_query` 虽然支持 two-level coarse routing 参数，但没有在查询循环前显式调用 `PrepareTwoLevelCoarseRouting(nprobe)`。因此第一次查询会触发 hierarchy build，并通过 `avg_coarse_hierarchy_build_ms` 摊入查询平均耗时。

本轮修复：

- 在创建 `OverlapScheduler` 之前设置 two-level coarse routing。
- 调用 `index.PrepareTwoLevelCoarseRouting(cfg.nprobe)`。
- 新增 JSON 指标 `two_level_coarse_warmup_ms`。
- 查询阶段的 `avg_coarse_hierarchy_build_ms` 应接近 0。

## 代码改动

- `benchmarks/bench_online_query.cpp`

## 实验设置

复用上一轮 two-level16 runner 的全部索引和参数：

- `amazon_esci`, `msmarco_passage`
- `total_bits=2,3,4`
- `topk=100`
- `nprobe=256`
- `two-level-coarse-routing=1`
- `two-level-coarse-budget-factor=16`
- `non-safeout-candidate-budget=400`
- `queries=1000`
- `reps=3`

实验目录：

`/home/zcq/VDB/test/pipeline_optimization_20260625/round1_twolevel_warmup`

runner：

`/home/zcq/VDB/test/pipeline_optimization_20260625/round1_twolevel_warmup/run_round1_twolevel_warmup.sh`

## 预期

1. recall 不变。
2. `avg_coarse_hierarchy_build_ms` 约为 0。
3. `two_level_coarse_warmup_ms` 记录一次性 warmup 成本。
4. 因为上一轮 build 成本只被摊到 1000 queries，平均延迟预计只会小幅下降。

## 结果

结果文件：

- raw CSV：`/home/zcq/VDB/test/pipeline_optimization_20260625/round1_twolevel_warmup/results/round1_twolevel_warmup_raw.csv`
- per-mode summary：`/home/zcq/VDB/test/pipeline_optimization_20260625/round1_twolevel_warmup/results/round1_twolevel_warmup_summary.csv`
- paired summary：`/home/zcq/VDB/test/pipeline_optimization_20260625/round1_twolevel_warmup/results/round1_twolevel_warmup_pair_summary.csv`

主结果：

| dataset | ex_bits | off ms | HP ms | HP speedup | off warmup ms | HP warmup ms | off hierarchy build / query | HP hierarchy build / query |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| amazon_esci | 1 | 1.8864 | 1.8198 | 1.0366 | 125.27 | 124.88 | 0.0000 | 0.0000 |
| amazon_esci | 2 | 1.8584 | 1.7856 | 1.0408 | 128.46 | 121.90 | 0.0000 | 0.0000 |
| amazon_esci | 3 | 1.8670 | 1.7881 | 1.0442 | 125.80 | 128.27 | 0.0000 | 0.0000 |
| msmarco_passage | 1 | 3.3554 | 3.1534 | 1.0640 | 234.62 | 250.25 | 0.0000 | 0.0000 |
| msmarco_passage | 2 | 3.2748 | 3.1184 | 1.0502 | 244.57 | 238.36 | 0.0000 | 0.0000 |
| msmarco_passage | 3 | 3.2396 | 3.1081 | 1.0423 | 246.27 | 244.17 | 0.0000 | 0.0000 |

与上一轮未 warmup 的 two-level16 结果相比：

| dataset | ex_bits | off delta | HP delta |
| --- | ---: | ---: | ---: |
| amazon_esci | 1 | -10.40% | -9.74% |
| amazon_esci | 2 | -9.52% | -10.28% |
| amazon_esci | 3 | -9.43% | -10.83% |
| msmarco_passage | 1 | -6.73% | -9.43% |
| msmarco_passage | 2 | -9.51% | -10.48% |
| msmarco_passage | 3 | -8.39% | -9.11% |

解释：

- 下降主要来自一次性 hierarchy build 不再摊入 1000 次查询平均值。
- ESCI warmup 约 `122-128 ms`，对应每查询约 `0.12 ms`。
- MSMARCO warmup 约 `238-250 ms`，对应每查询约 `0.24 ms`。
- 这与 coarse 阶段下降量一致；算法本身没有改变，recall 不变。

## 更新后的瓶颈

看 code-slab HugePage 模式，warmup 前移后瓶颈更集中到 probe：

| dataset | ex_bits | total ms | coarse | probe | Stage1 | Stage2 | submit | unaccounted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| amazon_esci | 1 | 1.8198 | 13.5% | 70.6% | 24.7% | 12.0% | 17.9% | 5.5% |
| amazon_esci | 2 | 1.7856 | 13.7% | 69.6% | 24.9% | 10.9% | 17.4% | 6.1% |
| amazon_esci | 3 | 1.7881 | 13.7% | 69.0% | 24.9% | 10.9% | 17.0% | 6.6% |
| msmarco_passage | 1 | 3.1534 | 10.6% | 73.4% | 36.8% | 8.5% | 17.2% | 9.7% |
| msmarco_passage | 2 | 3.1184 | 10.7% | 72.9% | 36.6% | 8.5% | 16.8% | 10.1% |
| msmarco_passage | 3 | 3.1081 | 10.7% | 72.8% | 37.0% | 8.8% | 16.0% | 10.0% |

结论：

- Round 1 达成目标，查询计时内的 hierarchy build 已归零。
- 下一轮优化应优先做 Stage1 build/preload-time block envelope。
- coarse topN 仍约 `0.24 ms` ESCI / `0.32 ms` MSMARCO，可作为后续低风险优化点。
- submit/I/O 仍占 `16-18%`，需要按 LAANN 映射继续做优先级 submit 与等待期 CPU work。
