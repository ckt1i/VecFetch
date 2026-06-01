# 验证记录

## 构建目录

- `build`

## 编译

```bash
cmake -S . -B build
cmake --build build -j 8 --target bench_vector_search bench_e2e test_overlap_scheduler test_rabitq_bench_calibration test_classify_masks
```

结果：以上目标编译通过。

## 单元测试

```bash
ctest --test-dir build -R '^(test_overlap_scheduler|test_classify_masks|test_rabitq_bench_calibration)$' --output-on-failure
```

结果：3/3 通过。

## COCO100k vector search

为了验证旧索引中存在 `segment.meta.crc_params` 但运行时不依赖 CRC 文件，创建了临时索引目录：

```bash
rm -rf /tmp/vectorretrival_no_crc_index_coco100k
mkdir -p /tmp/vectorretrival_no_crc_index_coco100k
for f in centroids.bin rotated_centroids.bin cluster.clu rotation.bin segment.meta build_metadata.json data.dat; do
  ln -s /home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90/$f \
    /tmp/vectorretrival_no_crc_index_coco100k/$f
done
```

运行命令：

```bash
./build/benchmarks/bench_vector_search \
  --base /home/zcq/VDB/data/coco_100k/image_embeddings.npy \
  --query /home/zcq/VDB/data/coco_100k/query_embeddings.npy \
  --gt /home/zcq/VDB/data/coco_100k/groundtruth_top10.npy \
  --image-ids /home/zcq/VDB/data/coco_100k/image_ids.npy \
  --index-dir /tmp/vectorretrival_no_crc_index_coco100k \
  --nlist 2048 --nprobe 64 --topk 10 --queries 1000 --bits 4 \
  --metric cosine --dynamic-safeout 1 \
  --outdir /home/zcq/VDB/test/remove_query_crc_early_stop/vector_search_no_crc_files
```

关键输出：

- `avg_probed = 64.00 / 64 clusters`
- `safeout_frontier_estimates_buffered = 98104`
- `safeout_frontier_estimates_merged = 98104`
- `safeout_frontier_updates = 42477`
- Stage1: `SafeIn=7`, `SafeOut=2682973`, `Uncertain=415932`
- Stage2: `SafeIn=164`, `SafeOut=317835`, `Uncertain=97933`
- 输出文件：`/home/zcq/VDB/test/remove_query_crc_early_stop/vector_search_no_crc_files/results.json`

说明：输出 JSON 中无 `crc` 或 `early` 字段。

## COCO100k E2E smoke

运行命令：

```bash
./build/benchmarks/bench_e2e \
  --dataset /home/zcq/VDB/data/coco_100k \
  --output /home/zcq/VDB/test/remove_query_crc_early_stop/e2e_smoke_no_crc_files \
  --index-dir /tmp/vectorretrival_no_crc_index_coco100k \
  --nlist 2048 --nprobe 64 --topk 10 --queries 100 --bits 4 \
  --clu-read-mode full_preload --use-resident-clusters 1 \
  --io-queue-depth 64 --cluster-submit-reserve 8 \
  --prefetch-depth 16 --refill-threshold 4 --refill-count 8 \
  --submit-batch 32 --fine-grained-timing 0 --hotpath-detailed-timing 0 \
  --fixed-vec-buffer-count 512 --enable-stage1-safein 1
```

关键输出：

- `FIXED: recall@10=0.9530`
- `FIXED: avg=0.399 ms`
- `FIXED: avg_probed_clusters=64.0`
- `FIXED: safeout_frontier_buffered_per_cluster=1.51`
- `FIXED: safeout_frontier_updates_per_cluster=0.66`
- 输出文件：`/home/zcq/VDB/test/remove_query_crc_early_stop/e2e_smoke_no_crc_files/coco_100k_20260601T214316/results.json`

说明：`results.json`、`config`、`pipeline_stats` 中均无 probing CRC 或 early-stop 字段。
