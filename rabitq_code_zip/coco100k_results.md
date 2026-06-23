# COCO100k bit=4 stage2 压缩实验结果

## 实验设置

数据集：

- base: `/home/zcq/VDB/data/coco_100k/image_embeddings.npy`
- query: `/home/zcq/VDB/data/coco_100k/query_embeddings.npy`
- GT: `/home/zcq/VDB/test/coco_gt_rebuilt_prefix1000_top100_20260605/gt_prefix1000_top100_image_ids.npy`

构建参数：

- `nlist=2048`
- `nprobe=64`
- `topk=10`
- `total_bits=4`
- `ex_bits=3`
- `epsilon_percentile=0.90`
- centroid/assignment 复用：
  - `/home/zcq/VDB/data/coco_100k/coco_centroid_2048.fvecs`
  - `/home/zcq/VDB/data/coco_100k/coco_cluster_id_2048.ivecs`

查询参数：

- `query_count=1000`
- `topk=10`
- `nprobe=64`
- `dynamic_safeout=1`
- `dynamic_safein=static`
- `non_safeout_candidate_budget=400`
- `fixed_vec_buffer_count=512`
- `rabitq_validation_mode=official_1_plus_n`

完整日志与 JSON：

- `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/`
- `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/`
- `/home/zcq/VDB/test/rabitq_code_zip_20260606/online_summary.csv`

## 索引大小

| layout | cluster.clu bytes | total index bytes | cluster.clu saving |
| --- | ---: | ---: | ---: |
| `split3_bitplanes` | 40,296,448 | 459,474,903 | baseline |
| `split3_trimmed_bitplanes` | 38,658,048 | 457,836,515 | 4.07% |
| `split3_zero_plane_elide` | 39,043,072 | 458,221,539 | 3.11% |

## Online query 结果

repeat 运行用于排除首次运行 cache/warmup 抖动：

| layout | R@10 | avg ms | QPS | p95 ms | peak RSS KiB | resident code bytes | avg rerank |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `split3_bitplanes` | 0.9504 | 0.3401 | 2940.4 | 0.4343 | 88,100 | 30,680,572 | 99.844 |
| `split3_trimmed_bitplanes` | 0.9504 | 0.3388 | 2951.4 | 0.4341 | 86,760 | 29,282,040 | 99.845 |
| `split3_zero_plane_elide` | 0.9504 | 0.3470 | 2881.9 | 0.4455 | 87,036 | 29,598,624 | 99.845 |

初次运行 baseline 明显慢于后续两次，主要是 warmup/cache 抖动；repeat 口径下更适合判断 layout 差异。

## 结论

1. `split3_trimmed_bitplanes` 不降低 recall，平均延迟没有损失，`cluster.clu` 降低 4.07%，resident code storage 降低 4.56%，peak RSS 降低约 1,340 KiB。
2. `split3_zero_plane_elide` 不降低 recall，但平均延迟比 baseline repeat 慢约 2.0%，并且空间收益低于 trimmed。原因与分布检查一致：COCO bit=4 的 64-bit plane word 只有约 6.90% 为全零。
3. 建议把 `split3_trimmed_bitplanes` 作为当前 bit=4 默认压缩候选，`split3_zero_plane_elide` 保留为消融/负例，不进入主结果。

## 复现命令

构建 trimmed：

```bash
./build/benchmarks/bench_build_index \
  --dataset /home/zcq/VDB/data/coco_100k \
  --output /home/zcq/VDB/test/rabitq_code_zip_20260606/build_outputs \
  --nlist 2048 --nprobe 64 --topk 10 --bits 4 \
  --rabitq-estimator-mode official_1_plus_n \
  --rabitq-total-bits 4 --rabitq-ex-bits 3 \
  --rabitq-exdata-layout split3_trimmed_bitplanes \
  --epsilon-percentile 0.90 \
  --centroids /home/zcq/VDB/data/coco_100k/coco_centroid_2048.fvecs \
  --assignments /home/zcq/VDB/data/coco_100k/coco_cluster_id_2048.ivecs \
  --calibration-samples 1000 --epsilon-samples 100 --max-iter 20 --seed 42
```

查询 trimmed：

```bash
./build/benchmarks/bench_e2e \
  --index-dir /home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_official_1_plus_n_total4_ex3_split3_trimmed_bitplanes_eps0.90 \
  --query-file /home/zcq/VDB/data/coco_100k/query_embeddings.npy \
  --gt-file /home/zcq/VDB/test/coco_gt_rebuilt_prefix1000_top100_20260605/gt_prefix1000_top100_image_ids.npy \
  --output /home/zcq/VDB/test/rabitq_code_zip_20260606/online/split3_trimmed_bitplanes_repeat \
  --query-count 1000 --topk 10 --nprobe 64 \
  --dynamic-safeout 1 --dynamic-safein static \
  --non-safeout-candidate-budget 400 \
  --fixed-vec-buffer-count 512 \
  --rabitq-validation-mode official_1_plus_n \
  --fine-grained-timing 0 --hotpath-detailed-timing 0
```
