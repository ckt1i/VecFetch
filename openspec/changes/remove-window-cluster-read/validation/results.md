# remove-window-cluster-read 验证记录

## 环境与构建

- 工作目录：`/home/zcq/VDB/VectorRetrival`
- 日期：`2026-06-03`
- 构建命令：`cmake --build build --target test_overlap_scheduler bench_e2e bench_vector_search`
- 构建结果：通过。
- 核心残留扫描：`rg` 确认 `include/`、`src/`、`benchmarks/`、`tests/` 中无 active `CluReadMode`、`clu_read_mode`、`use_resident_clusters`、`prefetch_depth`、`refill_threshold`、`refill_count`、`CLUSTER_BLOCK`、`PrefetchClusters`、`SubmitClusterRead`、`ProbeAndDrainInterleaved`、`ProbeResidentThinPath`、window/refill 状态成员和 query-time cluster parse/prefetch stats 引用。
- CLI 残留扫描：旧 cluster loading flags 只保留在 `bench_e2e` 的显式拒绝列表中。

## 代码路径变化

- `SearchConfig` 删除 window cluster read 相关配置，查询语义固定为 resident full-preload cluster views。
- `OverlapScheduler::Search` 不再在 window/full_preload 间分支，搜索开始前确保 `.clu` resident views 已存在；若未 preload，则执行 lazy `PreloadAllClusters()`，失败时返回清晰错误。
- 删除 query-time cluster block I/O：`CLUSTER_BLOCK`、`SubmitClusterRead`、`PrefetchClusters`、`ProbeAndDrainInterleaved`、`ready_clusters_`、`inflight_clusters_`、`next_to_submit_`、refill 逻辑均被移除。
- `DispatchCompletion()` 只保留 `VEC_ONLY`、`VEC_ALL`、`PAYLOAD` completion 行为。
- `bench_e2e` 在 measured query 前固定执行或确认 resident full preload，并继续输出 preload time、resident memory/bytes 和 hotpath timing。
- `bench_vector_search` 不再设置 prefetch/refill 配置；resident preload 由正式 scheduler 搜索路径保证。

## 参数兼容性检查

命令：

```bash
./build/benchmarks/bench_e2e \
  --dataset /home/zcq/VDB/data/coco_100k \
  --index-dir /home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90 \
  --queries 1 --nlist 2048 --nprobe 64 --topk 10 --bits 4 \
  --clu-read-mode window
```

结果：退出码 `1`，输出 `Unsupported cluster loading option --clu-read-mode: cluster.clu is always served from resident full preload.`

## COCO100k E2E

命令：

```bash
./build/benchmarks/bench_e2e \
  --dataset /home/zcq/VDB/data/coco_100k \
  --index-dir /home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90 \
  --queries 1000 --nlist 2048 --nprobe 64 --topk 10 --bits 4 \
  --epsilon-percentile 0.90 \
  --output /home/zcq/VDB/test/remove_window_cluster_read_coco_1000
```

- 输出目录：`/home/zcq/VDB/test/remove_window_cluster_read_coco_1000/coco_100k_20260603T180547`
- GT 来源：运行时 brute-force 计算。
- 是否计入 preload：query latency 不计入 preload；preload 单独统计。
- `cluster_mode=resident_full_preload`

| 指标 | 数值 |
| --- | ---: |
| Queries | 1000 |
| recall@1 | 0.9690 |
| recall@5 | 0.9576 |
| recall@10 | 0.9558 |
| avg / p50 / p95 / p99 latency ms | 0.3894 / 0.3839 / 0.5015 / 0.5660 |
| avg probed clusters | 64.0 |
| preload time ms | 76.2219 |
| preload bytes | 80,171,008 |
| resident memory bytes | 135,362,808 |
| resident parallel view build ms | 35.5692 |
| resident parallel view bytes | 61,701,120 |
| Stage1 SafeIn / SafeOut / Uncertain | 0.007 / 2682.973 / 97.933 |
| Stage2 SafeIn / SafeOut / Uncertain | 0.164 / 317.835 / 97.933 |
| false SafeOut avg | 0.442 |
| false SafeIn upper avg | 0.0 |
| total final SafeIn | 171 |
| safein payload prefetched avg | 0.171 |
| remaining payload fetches avg | 9.865 |

## COCO100k vector-only

命令：

```bash
./build/benchmarks/bench_vector_search \
  --base /home/zcq/VDB/data/coco_100k/image_embeddings.fvecs \
  --query /home/zcq/VDB/data/coco_100k/query_embeddings.fvecs \
  --queries 1000 \
  --image-ids /home/zcq/VDB/data/coco_100k/image_ids.npy \
  --index-dir /home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90 \
  --nlist 2048 --nprobe 64 --topk 10 --bits 4 \
  --dynamic-safeout 1 \
  --outdir /home/zcq/VDB/test/remove_window_cluster_read_coco_vector_1000
```

- 输出文件：`/home/zcq/VDB/test/remove_window_cluster_read_coco_vector_1000/results.json`

| 指标 | 数值 |
| --- | ---: |
| Queries | 1000 |
| recall@1 | 0.9690 |
| recall@5 | 0.9574 |
| recall@10 | 0.9558 |
| avg / p50 / p95 / p99 latency ms | 1.91837 / 1.65991 / 3.51271 / 4.94375 |
| avg probed clusters | 64 |
| Stage1 SafeIn / SafeOut / Uncertain | 7 / 2,682,973 / 415,932 |
| Stage1 false SafeIn / false SafeOut | 2 / 63 |
| Stage2 SafeIn / SafeOut / Uncertain | 164 / 317,835 / 97,933 |
| Stage2 false SafeIn / false SafeOut | 34 / 0 |
| final uncertain | 97,933 |
| vec-only / all / payload reads | 97,933 / 171 / 9,865 |
| safein payload prefetched | 171 |
| remaining payload fetches | 9,865 |
| SafeIn prefetch false rate | 21.0526% |
| SafeIn top-k coverage | 1.35% |

## MS MARCO E2E

命令：

```bash
./build/benchmarks/bench_e2e \
  --dataset /home/zcq/VDB/test/msmarco_fht_kac_adapter \
  --index-dir /home/zcq/VDB/test/data/MSMARCO/fht_kac_rotator \
  --output /home/zcq/VDB/test/remove_window_cluster_read_msmarco_200 \
  --gt-file /home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/gt/gt_top10.npy \
  --queries 200 --topk 10 --nlist 16384 --nprobe 256 --bits 4 \
  --metric cosine --query-only 0 --skip-gt 0 \
  --io-queue-depth 64 --cluster-submit-reserve 8 --submit-batch 32 \
  --fine-grained-timing 0 --hotpath-detailed-timing 0 --fixed-vec-buffer-count 512 \
  --assignment-mode single --coarse-builder superkmeans
```

- Adapter：`/home/zcq/VDB/test/msmarco_fht_kac_adapter`
- GT：`/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/gt/gt_top10.npy`
- 索引：`/home/zcq/VDB/test/data/MSMARCO/fht_kac_rotator`
- 输出目录：`/home/zcq/VDB/test/remove_window_cluster_read_msmarco_200/msmarco_fht_kac_adapter_20260603T180930`
- Adapter manifest 中的 query_rows 记录为 5，但实际 `query_embeddings.npy` 与 `query_ids.npy` 均为 200 行；本轮按实际可用 200 queries 执行。
- Build 口径：`nlist=16384`、`assignment_mode=single`、`coarse_builder=superkmeans`、`rotation_mode=fht_kac_rotator`、logical/effective dim `768/768`、metric `cosine`，索引 metadata 中 effective metric 为 `ip`。
- 是否计入 preload：query latency 不计入 preload；preload 单独统计。
- `cluster_mode=resident_full_preload`

| 指标 | 数值 |
| --- | ---: |
| Queries | 200 |
| recall@1 | 0.9600 |
| recall@5 | 0.9530 |
| recall@10 | 0.9420 |
| avg / p50 / p95 / p99 latency ms | 4.6859 / 4.5513 / 6.2771 / 7.1475 |
| avg probed clusters | 256.0 |
| preload time ms | 9019.6400 |
| preload bytes | 8,793,952,256 |
| resident memory bytes | 16,539,397,640 |
| resident parallel view build ms | 4005.1632 |
| resident parallel view bytes | 7,688,632,320 |
| Stage1 SafeIn / SafeOut / Uncertain | 0.000 / 155875.065 / 740.155 |
| Stage2 SafeIn / SafeOut / Uncertain | 0.090 / 81.220 / 740.155 |
| false SafeOut avg | 0.580 |
| false SafeIn upper avg | 0.0 |
| total final SafeIn | 18 |
| safein payload prefetched avg | 0.090 |
| remaining payload fetches avg | 9.910 |

## 回归测试

命令：

```bash
ctest --test-dir build -R '^(test_overlap_scheduler|test_io_uring_reader|test_pread_fallback_reader|test_buffer_pool|test_rerank_consumer|test_ivf_index|test_cluster_store)$' --output-on-failure
```

结果：

| Target | 结果 |
| --- | --- |
| `test_overlap_scheduler` | 通过 |
| `test_io_uring_reader` | 通过 |
| `test_pread_fallback_reader` | 通过 |
| `test_buffer_pool` | 通过 |
| `test_rerank_consumer` | 通过 |
| `test_ivf_index` | 通过 |
| `test_cluster_store` | 失败 |

`test_cluster_store` 失败项：

- `ClusterStoreTest.ParseClusterBlock_Bits4MatchesLoadedData`：`parsed.exrabitq_entries` 为 null。
- `ClusterStoreTest.ParseClusterBlock_V9RawAddressTableSupportsGaps`：期望 `reader.file_version()==9`，实际为 `11`。

判断：这两个失败位于 storage parse/version 单测层，和本次删除 query-time window cluster read 的 scheduler active path 不同。本 change 不修改 `.clu` 文件格式，也不要求重建 COCO/MS MARCO 既有索引。

## 对比与风险

- 未在 change 目录中找到可机器读取的删除前同参结果，因此无法给出严格 pre/post 数值回归归因。
- COCO100k 与 MS MARCO 均复用既有索引，并对齐历史主锚点参数；MS MARCO 旧 `test_configs.txt` 记录为 1000 queries，但当前 adapter 文件实际只有 200 queries，本轮结果不能与 1000-query 历史日志直接比较。
- 如果后续观察到 latency 变化，主要归因口径应分离为 resident preload 是否计入、resident cluster probe、raw vector I/O、SafeOut frontier 维护和 benchmark query-count/GT 差异。本轮 query latency 均不计入 preload。
- 文件格式风险较低：`.clu`、`data.dat`、`segment.meta` 没有 schema 变更，删除的是 query-time window read 控制路径；现有索引仍可复用。
