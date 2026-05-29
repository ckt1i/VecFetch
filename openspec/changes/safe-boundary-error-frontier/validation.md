# 验证记录：COCO100k 仅向量搜索

## 正式结果

本轮使用预建索引：

```bash
./build/benchmarks/bench_vector_search \
  --base /home/zcq/VDB/data/coco_100k/image_embeddings.fvecs \
  --query /home/zcq/VDB/data/coco_100k/query_embeddings.fvecs \
  --image-ids /home/zcq/VDB/data/coco_100k/image_ids.npy \
  --assignments /home/zcq/VDB/data/coco_100k/coco_cluster_id_2048.ivecs \
  --index-dir /home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90 \
  --nlist 2048 \
  --nprobe 64 \
  --topk 10 \
  --bits 4 \
  --metric cosine \
  --crc 1 \
  --early-stop 1 \
  --enable-stage1-safein 1 \
  --queries 1000 \
  --outdir openspec/changes/safe-boundary-error-frontier/validation/coco100k_nlist2048_nprobe64_q1000_bruteforce_imageids
```

输出目录：

`openspec/changes/safe-boundary-error-frontier/validation/coco100k_nlist2048_nprobe64_q1000_bruteforce_imageids/`

## Summary

- 数据集：COCO100k
- 索引：`index_fkmeans_2048_bits4_eps0.90`
- 配置：`nlist=2048`、`nprobe=64`、`topk=10`、`bits=4`
- 查询数：1000
- GT：benchmark 内部用 base/query `.fvecs` 现场 brute-force L2 生成，并用 `image_ids.npy` 对齐索引 payload
- recall@1：0.9160
- recall@5：0.9012
- recall@10：0.8986
- 平均延迟：3.235 ms
- p50 / p95 / p99：3.637 / 4.643 / 5.156 ms
- 平均 probe：55.21 / 64
- early stop rate：0.1480

## SafeIn / SafeOut / Uncertain

Stage1 FastScan：

- SafeIn：3
- SafeOut：1,505,096
- Uncertain：1,160,154
- False SafeIn：1
- False SafeOut：1

Stage2 4-bit ExRaBitQ：

- SafeIn：84
- SafeOut：777,277
- Uncertain：382,793
- False SafeIn：11
- False SafeOut：0
- Final Uncertain：382,793

## SafeIn d_k percentile sweep

在相同 COCO100k / `nlist=2048` / `nprobe=64` / 1000 query / `.fvecs + brute-force GT + image_ids` 口径下，额外测试：

```bash
for p in 0.90 0.95 0.97; do
  ./build/benchmarks/bench_vector_search \
    --base /home/zcq/VDB/data/coco_100k/image_embeddings.fvecs \
    --query /home/zcq/VDB/data/coco_100k/query_embeddings.fvecs \
    --image-ids /home/zcq/VDB/data/coco_100k/image_ids.npy \
    --assignments /home/zcq/VDB/data/coco_100k/coco_cluster_id_2048.ivecs \
    --index-dir /home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90 \
    --nlist 2048 \
    --nprobe 64 \
    --topk 10 \
    --bits 4 \
    --metric cosine \
    --crc 1 \
    --early-stop 1 \
    --enable-stage1-safein 1 \
    --queries 1000 \
    --safein-dk-percentile "$p" \
    --safein-dk-samples 1000 \
    --outdir "openspec/changes/safe-boundary-error-frontier/validation/coco100k_nlist2048_nprobe64_q1000_safein_dk_p${p/./}"
done
```

| safein_dk_percentile | calibrated safein_d_k | recall@10 | latency avg ms | Stage1 SafeIn | Stage1 SafeOut | Stage1 Uncertain | Stage1 false SafeIn | Stage1 false SafeOut | Stage2 SafeIn | Stage2 SafeOut | Stage2 Uncertain | Stage2 false SafeIn | Stage2 false SafeOut |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0.90 | 1.30768 | 0.8986 | 3.221 | 116 | 1,505,096 | 1,160,041 | 55 | 1 | 1,239 | 777,277 | 381,525 | 280 | 0 |
| 0.95 | 1.29624 | 0.8986 | 3.216 | 58 | 1,505,096 | 1,160,099 | 25 | 1 | 692 | 777,277 | 382,130 | 134 | 0 |
| 0.97 | 1.28644 | 0.8986 | 3.225 | 35 | 1,505,096 | 1,160,122 | 13 | 1 | 410 | 777,277 | 382,435 | 71 | 0 |

观察：在这三组中，SafeOut 和 recall@10 基本不变；`safein_d_k` 越小，SafeIn 数量和 false SafeIn 数量同步下降。输出目录分别为：

- `validation/coco100k_nlist2048_nprobe64_q1000_safein_dk_p090/`
- `validation/coco100k_nlist2048_nprobe64_q1000_safein_dk_p095/`
- `validation/coco100k_nlist2048_nprobe64_q1000_safein_dk_p097/`

## Baseline 对照

已有最近 COCO 诊断日志位于 `/home/zcq/VDB/test/diag/bench_vector_search_coco_*.log`，但不是完全相同口径：旧日志使用另一套临时索引 `/home/zcq/VDB/test/tmp/bench_vector_search_coco_payload_idx`，查询数为 200，并启用了 split epsilon / safein_d_k override。

最接近的一条旧日志 `bench_vector_search_coco_dk0.99_ein0.99_eout0.97.log`：

- recall@10：0.6035
- Stage1：SafeIn 21，SafeOut 535,109，Uncertain 18,171
- Stage2：SafeIn 10，SafeOut 3,600，Uncertain 14,561
- False SafeOut 未在该旧日志中输出

因此本轮结果可用于确认新划分逻辑在指定预建索引上的行为，但不应把上述旧日志视为严格 A/B。

## 口径说明

曾尝试直接使用 `groundtruth_top10.npy`。该文件是 row id；如果同时传入 `--image-ids`，benchmark 会把 GT 转成 image id。对于当前预建索引 payload，正确 recall 口径是用 `.fvecs` 输入现场 brute-force GT，并传入 `image_ids.npy` 让 GT 与结果 payload 对齐。
