# MSMARCO Padded-Hadamard Storage Split 结果归档

## 一句话结论
`logical_dim` raw-plane 存储修复后，`padded-Hadamard` 方案保留 `avg_query`/`recall` 性能收益的同时，将 `data.dat` footpring 从约 `72.4GB` 回落到约 `34GB`，整体索引规模从约 `84GB` 回落到约 `46GB`。

## 验证对比

- 变更版本（已复用 centroids/assignments）:
  - centroids: `/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings/msmarco_passage_centroid_16384.fvecs`
  - assignments: `/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings/msmarco_passage_cluster_id_16384.ivecs`

- 数据集适配目录:
  - `/home/zcq/VDB/test/data/msmarco_passage_adapter_full`

- 基线（`random_matrix`, `logical/effective=768/768`）:
  - index: `/home/zcq/VDB/test/msmarco_superkmeans_random_full/index`
  - recall@10: `0.9615`
  - avg_query: `14.1218 ms`
  - data.dat: `34G`
  - cluster.clu: `8.79G`

- `padded-Hadamard`（`pad-to-pow2=1`, `logical/effective=768/1024`, `data.dat=logical_dim`）:
  - index: `/home/zcq/VDB/test/logical_dim_data_storage_for_padded_hadamard/msmarco_passage_adapter_full_20260424T191013/index`
  - recall@10: `0.9615`
  - avg_query: `7.106 ms`
  - data.dat: `34G`
  - cluster.clu: `11.66G`

- 对比命令（固定操作点）:
  - full rebuild: 使用已有 centroids/assignments，`--pad-to-pow2 1`，`--nlist 16384 --nprobe 256 --bits 4`，`--gt-file /home/zcq/VDB/test/data/msmarco_passage_adapter_full/gt_top10.npy`
  - full-e2e query: `bench_e2e --use-resident-clusters 1 --clu-read-mode full_preload`

## 归档约束（后续复用）
- raw storage 读取/写入契约: `data.dat` 使用 `logical_dim`
- code/cluster/prepare 契约: `effective_dim=1024`（padded-Hadamard）
- 复用原有 centroids，禁止在该变更中重训 clustering
