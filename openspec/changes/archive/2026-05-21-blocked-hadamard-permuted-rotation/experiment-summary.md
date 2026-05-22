# MSMARCO blocked_hadamard_permuted 实验摘要

## 配置

- 数据集: `/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings`
- 构建适配数据: `/home/zcq/VDB/test/msmarco_blocked_hadamard_adapter`
- `nlist = 16384`
- `nprobe = 256`
- `topk = 10`
- `queries = 1000`
- `metric = cosine`
- `bits = 1`
- CRC: `enabled`, `alpha = 0.02`, `solver = brent`

## 结果

| 模式 | logical/effective dim | recall@10 | avg latency (ms) | p95 (ms) | rotation.bin | rotated_centroids.bin | cluster.clu | index total |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `random_matrix` | `768 / 768` | 参考基线 | `~13.0` | - | - | - | - | - |
| `hadamard_padded` | `768 / 1024` | `0.9615` | `7.1056` | `11.5983` | `4,195,333` | `67,108,864` | `11,664,814,080` | `48,028,829,564` |
| `blocked_hadamard_permuted` | `768 / 768` | `0.9570` | `10.9495` | `25.4219` | `2,363,161` | `50,331,648` | `1,065,050,112` | `37,393,679,050` |

## 实验产物

- blocked build-only 索引:
  `/home/zcq/VDB/test/msmarco_blocked_hadamard_debug2/run/msmarco_blocked_hadamard_adapter_20260424T222541/index`
- blocked 评测结果:
  `/home/zcq/VDB/test/msmarco_blocked_hadamard_eval/msmarco_blocked_hadamard_adapter_20260424T223751/results.json`
- padded 对照结果:
  `/home/zcq/VDB/test/logical_dim_data_storage_for_padded_hadamard_fulle2e_v3/msmarco_passage_adapter_full_20260424T193152/results.json`

## 结论

- `blocked_hadamard_permuted` 成功避免了 `768 -> 1024` padding，`effective_dim` 保持为 `768`。
- 存储上收益明显:
  - `rotation.bin` 较 `hadamard_padded` 更小
  - `rotated_centroids.bin` 从 `67.1 MB` 降到 `50.3 MB`
  - `cluster.clu` 从 `11.66 GB` 降到 `1.07 GB`
  - 总索引体积从 `48.03 GB` 降到 `37.39 GB`
- 但查询性能没有达到 `hadamard_padded`:
  - 平均延迟从 `7.1056 ms` 上升到 `10.9495 ms`
  - `recall@10` 从 `0.9615` 下降到 `0.9570`
- 因此在当前实现下，`blocked_hadamard_permuted` 更像是“存储优化方案”，不是对 `padded_hadamard` 的直接性能替代。
