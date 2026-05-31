# DiskANN PQ0 实验简报

- started_at: `2026-05-30T03:29:18.055397+00:00`
- finished_at: `2026-05-30T06:04:28.259799+00:00`
- 口径: `build_disk_index/search_disk_index` stock CLI, `--PQ_disk_bytes 0`, SSD 存原始向量。
- 注意: DiskANN disk search 默认仍加载 search-memory PQ，用于 graph traversal；本轮未修改源码。
- combined_csv: `/home/zcq/VDB/baselines/results/diskann_cpp_pareto_pq0_combined_20260530.csv`
- frontier_csv: `/home/zcq/VDB/baselines/results/diskann_cpp_pareto_pq0_frontier_20260530.csv`
- targets_csv: `/home/zcq/VDB/baselines/results/diskann_cpp_pareto_pq0_targets_20260530.csv`

## Recall 覆盖
- coco_100k: valid=64, recall@10=[0.821900, 0.999700]
- msmarco_passage: valid=38, recall@10=[0.641500, 0.996100]

## 索引 Manifest 校验
- coco_100k R=64 L=100 pq_disk_bytes=0 ssd_vector_storage=raw path=/home/zcq/VDB/baselines/data/diskann_disk_coco_100k_R64_L100_PQ0/diskann_manifest.json
- msmarco_passage R=32 L=50 pq_disk_bytes=0 ssd_vector_storage=raw path=/home/zcq/VDB/baselines/data/diskann_disk_msmarco_passage_R32_L50_PQ0/diskann_manifest.json
- msmarco_passage R=96 L=200 pq_disk_bytes=0 ssd_vector_storage=raw path=/home/zcq/VDB/baselines/data/diskann_disk_msmarco_passage_R96_L200_PQ0/diskann_manifest.json

## 无效点
- none

