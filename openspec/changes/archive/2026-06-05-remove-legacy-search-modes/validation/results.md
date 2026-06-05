# remove-legacy-search-modes 验证记录

## 当前状态

- 已完成 HNSW coarse routing、冗余/RAIR assignment、手动 padded/blocked Hadamard 和用户显式 FHT-Kac opt-in 的正式路径清理。
- FlatBuffers schema 字段保留为 legacy compatibility；新建索引固定写入 `assignment_mode=SINGLE`、`assignment_factor=1`。
- 查询打开路径会拒绝 redundant/RAIR metadata，以及 `hadamard_padded` / `blocked_hadamard_permuted` legacy rotation metadata。
- COCO100k / MS MARCO / 五数据集性能回归尚未完成，暂不计入任务完成。

## Baseline audit

当前主线命令口径来自 `test_configs.txt`：

- COCO100k `bench_e2e`：

```bash
./build/benchmarks/bench_e2e \
  --dataset /home/zcq/VDB/data/coco_100k \
  --output /home/zcq/VDB/test/frontier_upper_s2heap_coco100k_safein_dk_p0.95_manual \
  --index-dir /home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90 \
  --nlist 2048 --nprobe 64 --topk 10 --queries 1000 --bits 4 \
  --io-queue-depth 64 --cluster-submit-reserve 8 --initial-prefetch 16 \
  --submit-batch 32 --early-stop 0 --crc 1 \
  --fine-grained-timing 0 --hotpath-detailed-timing 0 \
  --fixed-vec-buffer-count 512 --enable-stage1-safein 1 \
  --safein-dk-percentile 0.95
```

- MS MARCO `bench_e2e`：

```bash
./build/benchmarks/bench_e2e \
  --dataset /home/zcq/VDB/test/msmarco_fht_kac_adapter \
  --index-dir /home/zcq/VDB/test/data/MSMARCO/fht_kac_rotator \
  --output /home/zcq/VDB/test/hotpath_timing_ablation/recall_full \
  --query-only 0 --skip-gt 0 --queries 1000 --topk 10 --nprobe 256 \
  --io-queue-depth 64 --cluster-submit-reserve 8 --initial-prefetch 16 \
  --submit-batch 32 --crc 1 --fine-grained-timing 0 \
  --hotpath-detailed-timing 0 --fixed-vec-buffer-count 512
```

- COCO100k vector-only：

```bash
./bench_vector_search \
  --base /home/zcq/VDB/data/coco_100k/image_embeddings.fvecs \
  --query /home/zcq/VDB/data/coco_100k/query_embeddings.fvecs \
  --nlist 2048 --nprobe 64 --topk 10 --bits 4 --queries 1000 --crc 1 \
  --tmp-dir /tmp/bench_vs_coco --epsilon-samples 500 \
  --epsilon-percentile 0.95 --crc-alpha 0.01
```

`remove-window-cluster-read` 已完成并记录：正式 cluster-side query data path 固定为 resident full-preload；`bench_e2e` 继续拒绝 `--clu-read-mode`、`--use-resident-clusters`、`--prefetch-depth`、`--refill-threshold` 和 `--refill-count`。

Pre-cleanup build passed before code deletion:

```bash
cmake --build build --target bench_e2e bench_vector_search -j 8
```

## Code cleanup summary

- `SearchConfig` 和 `SearchStats` 删除 HNSW coarse routing 配置与统计字段。
- `IvfIndex` 删除 HNSW centroid routing runtime，`FindNearestClusters()` 只保留 exact 和 two-level coarse routing。
- `IvfBuilderConfig` 删除冗余/RAIR assignment 和手动 rotation mode knobs；构建端固定 single assignment。
- `.clu` membership 只来自 primary `assignments_`。
- 非 2 的幂维度自动使用 FHT-Kac；2 的幂维度继续使用 Hadamard fast path。
- `bench_e2e` / `bench_vector_search` 删除或明确拒绝 legacy CLI，并清理相关 JSON/CSV/per-query 输出字段。
- `run_blocked_hadamard_msmarco.py` 已删除；`run_fht_kac_msmarco.py` 改为 FHT-Kac-only validation helper。

## Build and unit tests

删除后受影响目标编译通过：

```bash
cmake --build build --target \
  bench_e2e bench_vector_search test_ivf_builder test_ivf_index \
  test_overlap_scheduler test_rabitq_rotation test_rabitq_estimator -j 8
```

目标测试结果：

| 命令 | 结果 |
| --- | --- |
| `./build/test_ivf_builder` | 14/14 passed |
| `./build/test_ivf_index` | 13/13 passed |
| `./build/test_overlap_scheduler` | 20/20 passed |
| `./build/test_rabitq_rotation` | 30/30 passed |
| `./build/test_rabitq_estimator` | 22/22 passed |

新增测试覆盖：

- `FhtKacRotatorBuildAndOpen_768D_NoPadding`：验证 768 维构建记录 `rotation_mode="fht_kac_rotator"`，且不出现 `hadamard_padded`、`blocked_hadamard_permuted` 或 `random_matrix`。
- `OpenRejectsLegacyBlockedHadamardMetadata`：验证旧 blocked Hadamard metadata 在 `Open()` 阶段被明确拒绝。

## CLI guard smoke tests

以下删除参数均明确返回退出码 `1`：

| 命令 | 结果 |
| --- | --- |
| `./build/benchmarks/bench_e2e --hnsw-coarse-routing 1` | rejected as unsupported legacy search/build option |
| `./build/benchmarks/bench_e2e --assignment-mode single` | rejected as unsupported legacy search/build option |
| `./build/benchmarks/bench_e2e --blocked-hadamard-permuted 1` | rejected as unsupported legacy search/build option |
| `./build/benchmarks/bench_vector_search --pad-to-pow2 1` | rejected as unsupported legacy option |

## Residual reference audit

Audit command:

```bash
rg -n -g '!build/**' -- \
  "--pad-to-pow2|--blocked-hadamard|--fht-kac|--assignment-|--rair|--save-secondary|hnsw|HNSW|coarse_hnsw|assignment_factor|save_secondary|pad_non_power|use_blocked|use_fht_kac_rotator" \
  include src benchmarks tests test_configs.txt openspec/changes/remove-legacy-search-modes
```

允许残留：

- OpenSpec 文档和任务中描述被删除的旧能力。
- `bench_e2e` / `bench_vector_search` 的 legacy option rejection strings。
- FlatBuffers schema compatibility 字段读取和 `assignment_factor=1` 写入/测试。
- schema 测试中 `hnsw_params (none)` 注释。

未发现 maintained benchmark command 仍主动启用 HNSW、RAIR/redundant assignment、secondary assignment save、manual pad-to-pow2 或 blocked Hadamard。

## Validation benchmarks

### COCO100k `bench_e2e`

当前验证命令：

```bash
./build/benchmarks/bench_e2e \
  --dataset /home/zcq/VDB/data/coco_100k \
  --index-dir /home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90 \
  --output /home/zcq/VDB/test/remove_legacy_search_modes_coco_1000 \
  --nlist 2048 --nprobe 64 --topk 10 --queries 1000 --bits 4 \
  --query-only 0 --skip-gt 0 \
  --io-queue-depth 64 --cluster-submit-reserve 8 --submit-batch 32 \
  --fine-grained-timing 0 --hotpath-detailed-timing 0 \
  --fixed-vec-buffer-count 512
```

结果文件：

```text
/home/zcq/VDB/test/remove_legacy_search_modes_coco_1000/coco_100k_20260603T191833/results.json
```

对比基准：

```text
/home/zcq/VDB/test/remove_window_cluster_read_coco_1000/coco_100k_20260603T180547/results.json
```

| 指标 | pre-cleanup mainline | remove-legacy-search-modes |
| --- | ---: | ---: |
| queries | 1000 | 1000 |
| index | `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90` | same |
| GT | computed brute force | computed brute force |
| rotation | `hadamard` | `hadamard` |
| coarse routing mode | 0 exact | 0 exact |
| recall@10 | 0.9558 | 0.9558 |
| avg query ms | 0.3894 | 0.3698 |
| p95 query ms | 0.5015 | 0.4765 |
| preload ms | 76.2219 | 83.5958 |
| avg SafeIn / SafeOut / Uncertain | 0.007 / 2682.973 / 97.933 | 0.007 / 2682.973 / 97.933 |
| avg S2 SafeIn / SafeOut / Uncertain | 0.164 / 317.835 / 97.933 | 0.164 / 317.835 / 97.933 |
| probe prepare / stage1 / stage2 / submit ms | 0.0526 / 0.0399 / 0.0921 / 0.0267 | 0.0514 / 0.0393 / 0.0907 / 0.0236 |

结论：同一 index、同一 query count、同一 GT 口径下 recall 完全一致；平均查询延迟和 p95 均未回退。

### MS MARCO `bench_e2e`

当前验证命令：

```bash
./build/benchmarks/bench_e2e \
  --dataset /home/zcq/VDB/test/msmarco_fht_kac_adapter_1000 \
  --index-dir /home/zcq/VDB/test/data/MSMARCO/fht_kac_rotator \
  --output /home/zcq/VDB/test/remove_legacy_search_modes_msmarco_1000 \
  --nlist 16384 --nprobe 256 --topk 10 --queries 1000 --bits 4 \
  --gt-file /home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/gt/gt_top100.npy \
  --query-only 0 --skip-gt 0 \
  --io-queue-depth 64 --cluster-submit-reserve 8 --submit-batch 32 \
  --fine-grained-timing 0 --hotpath-detailed-timing 0 \
  --fixed-vec-buffer-count 512
```

结果文件：

```text
/home/zcq/VDB/test/remove_legacy_search_modes_msmarco_1000/msmarco_fht_kac_adapter_1000_20260603T192002/results.json
```

最近 pre-cleanup mainline 基准：

```text
/home/zcq/VDB/test/remove_window_cluster_read_msmarco_200/msmarco_fht_kac_adapter_20260603T180930/results.json
```

注意：最近基准只覆盖 200 queries，且使用 `gt_top10.npy`；本轮按后续正式实验口径使用 1000 queries 和 `gt_top100.npy`，因此只作为 sanity comparison，不能做逐项严格回归判定。

| 指标 | recent mainline sanity | remove-legacy-search-modes |
| --- | ---: | ---: |
| queries | 200 | 1000 |
| adapter | `/home/zcq/VDB/test/msmarco_fht_kac_adapter` | `/home/zcq/VDB/test/msmarco_fht_kac_adapter_1000` |
| index | `/home/zcq/VDB/test/data/MSMARCO/fht_kac_rotator` | same |
| GT | `gt_top10.npy` | `gt_top100.npy` |
| rotation | `fht_kac_rotator` | `fht_kac_rotator` |
| coarse routing mode | 0 exact | 0 exact |
| recall@10 | 0.9420 | 0.9552 |
| avg query ms | 4.6859 | 4.5494 |
| p95 query ms | 6.2771 | 5.7222 |
| preload ms | 9019.6400 | 8261.0405 |
| avg SafeIn / SafeOut / Uncertain | 0.000 / 155875.065 / 740.155 | 0.010 / 157884.613 / 745.874 |
| avg S2 SafeIn / SafeOut / Uncertain | 0.090 / 81.220 / 740.155 | 0.340 / 86.245 / 745.874 |
| probe prepare / stage1 / stage2 / submit ms | 0.3515 / 1.2139 / 0.1631 / 1.0081 | 0.3498 / 1.2059 / 0.1708 / 0.9135 |

结论：FHT-Kac index 可在 1000-query / `gt_top100.npy` 正式口径下正常服务；recall 和延迟相对最近 sanity baseline 均无明显回退。由于 query count 和 GT 文件不同，本条记录标记为 non-identical-parameter sanity comparison。

### COCO100k `bench_vector_search`

当前验证命令：

```bash
./build/benchmarks/bench_vector_search \
  --base /home/zcq/VDB/data/coco_100k/image_embeddings.fvecs \
  --query /home/zcq/VDB/data/coco_100k/query_embeddings.fvecs \
  --image-ids /home/zcq/VDB/data/coco_100k/image_ids.npy \
  --index-dir /home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90 \
  --nlist 2048 --nprobe 64 --topk 10 --bits 4 --queries 1000 \
  --dynamic-safeout 1 \
  --outdir /home/zcq/VDB/test/remove_legacy_search_modes_coco_vector_reuse_ids_1000
```

结果文件：

```text
/home/zcq/VDB/test/remove_legacy_search_modes_coco_vector_reuse_ids_1000/results.json
```

对比基准：

```text
/home/zcq/VDB/test/remove_window_cluster_read_coco_vector_1000/results.json
```

| 指标 | pre-cleanup mainline | remove-legacy-search-modes |
| --- | ---: | ---: |
| queries | 1000 | 1000 |
| nlist / nprobe / topk / bits | 2048 / 64 / 10 / 4 | 2048 / 64 / 10 / 4 |
| index source | reused accepted COCO index | reused accepted COCO index |
| image id mapping | external ids | external ids |
| recall@10 | 0.9558 | 0.9558 |
| avg latency ms | 1.91837 | 1.90766 |
| p95 latency ms | 3.51271 | 3.48652 |
| S1 SafeIn / SafeOut / Uncertain | 7 / 2682973 / 415932 | 7 / 2682973 / 415932 |
| S2 SafeIn / SafeOut / Uncertain | 164 / 317835 / 97933 | 164 / 317835 / 97933 |

结论：删除 `--pad-to-pow2` 后，COCO vector-only 复用 accepted index 的验证口径保持 recall 一致，延迟在噪声范围内略低。

不可比记录：`/home/zcq/VDB/test/remove_legacy_search_modes_coco_vector_1000/results.json` 使用临时重建 index 且显式设置 `--epsilon-percentile 0.95`，recall@10=0.8334；该结果证明当前 CLI 不依赖 `--pad-to-pow2`，但因校准口径不同，不纳入回归结论。

## Validation conclusion

- COCO100k E2E：同参同索引 recall 无变化，latency 无回退。
- COCO100k vector-only：同参同索引 recall 无变化，latency 无回退。
- MS MARCO E2E：1000-query / `gt_top100.npy` FHT-Kac 正式口径通过；与最近 200-query sanity run 相比无明显性能损失。
- 所有正式验证均未使用 HNSW、redundant/RAIR assignment、secondary assignment、manual pad-to-pow2 或 blocked Hadamard 参数。
- 正式 cluster-side query data path 均为 resident full-preload。
