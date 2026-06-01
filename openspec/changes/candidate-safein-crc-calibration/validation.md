# 验证记录：candidate-level SafeIn CRC

## COCO100K 配置

使用预建索引和 brute-force GT 口径：

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
  --queries 1000
```

第一轮使用：

```bash
--candidate-safein-crc 1 \
--candidate-safein-beta-sweep 0.05,0.10,0.20 \
--candidate-safein-beta 0.05 \
--candidate-safein-artifact-output openspec/changes/candidate-safein-crc-calibration/validation/coco100k_candidate_crc_lowbeta_artifact.json
```

随后使用同一 artifact 分别加载 `--candidate-safein-beta 0.10` 和 `0.20` 重跑线上查询。

## 离线 replay 校准结果

阈值按照 `final` SafeIn 统计选择。`beta` 表示条件比例 `falseSafeIn / SafeIn`，不是 `falseSafeIn / all_candidates`。置信区间为 query-block bootstrap 95% CI，用于表示该比例在查询采样上的不确定性。

| beta | threshold T | calib final SafeIn | calib false | calib false/SafeIn | calib 95% CI | valid final SafeIn | valid false | valid false/SafeIn | valid 95% CI |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0.05 | 1.232572 | 40 | 2 | 0.050000 | [0.000000, 0.109091] | 12 | 0 | 0.000000 | [0.000000, 0.000000] |
| 0.10 | 1.236284 | 44 | 4 | 0.090909 | [0.000000, 0.157895] | 19 | 0 | 0.000000 | [0.000000, 0.000000] |
| 0.20 | 1.241070 | 62 | 12 | 0.193548 | [0.000000, 0.294118] | 22 | 0 | 0.000000 | [0.000000, 0.000000] |

## 线上查询结果

线上比较使用 `U_i = d_hat_i + e_i < T`，每个 beta 单独跑一次 `bench_vector_search`。

| beta | threshold T | recall@10 | latency avg ms | avg probed | early stop | Stage1 SafeIn | Stage1 false | Stage2 SafeIn | Stage2 false | total SafeIn | total false | online false/SafeIn |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 0.05 | 1.232572 | 0.9108 | 1.813 | 56.70 | 0.1240 | 1 | 0 | 30 | 1 | 31 | 1 | 0.032258 |
| 0.10 | 1.236284 | 0.9108 | 1.808 | 56.70 | 0.1240 | 1 | 0 | 37 | 2 | 38 | 2 | 0.052632 |
| 0.20 | 1.241070 | 0.9108 | 1.810 | 56.70 | 0.1240 | 1 | 0 | 46 | 4 | 47 | 4 | 0.085106 |

输出目录：

- `openspec/changes/candidate-safein-crc-calibration/validation/coco100k_candidate_crc_beta005/`
- `openspec/changes/candidate-safein-crc-calibration/validation/coco100k_candidate_crc_beta010/`
- `openspec/changes/candidate-safein-crc-calibration/validation/coco100k_candidate_crc_beta020/`

## 与旧 SafeIn d_k sweep 对比

旧 exact/RabitQ kth SafeIn 阈值 sweep 结果来自 `safe-boundary-error-frontier` 的相同 COCO100K / `nprobe=64` / 1000 query 口径。

| method | parameter | threshold | recall@10 | latency avg ms | total SafeIn | total false | false/SafeIn |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| old safein_dk | p=0.90 | 1.30768 | 0.8986 | 3.221 | 1,355 | 335 | 0.247232 |
| old safein_dk | p=0.95 | 1.29624 | 0.8986 | 3.216 | 750 | 159 | 0.212000 |
| old safein_dk | p=0.97 | 1.28644 | 0.8986 | 3.225 | 445 | 84 | 0.188764 |
| candidate CRC | beta=0.05 | 1.232572 | 0.9108 | 1.813 | 31 | 1 | 0.032258 |
| candidate CRC | beta=0.10 | 1.236284 | 0.9108 | 1.808 | 38 | 2 | 0.052632 |
| candidate CRC | beta=0.20 | 1.241070 | 0.9108 | 1.810 | 47 | 4 | 0.085106 |

观察：

- 低 `beta` sweep 下 candidate CRC 阈值低于旧 `d_k` 分位阈值，SafeIn 数量很少，但线上 falseSafeIn/SafeIn 被明显压低。
- 三个低 `beta` 点的 recall@10 都为 0.9108，高于旧 sweep 记录；延迟约 1.81 ms，主要收益仍来自 CRC early-stop/SafeOut，而不是 SafeIn 数量。
- validation final split 的 falseSafeIn 为 0，但 SafeIn 样本数只有 12/19/22，CI 退化到 0；这说明当前低 beta 区间过保守，后续若要兼顾 SafeIn 数量和 false ratio，应在 `0.20` 到旧 `0.80` 之间继续加密 sweep。
