# SafeIn d_k 静态阈值验证分解报告

## 结论

本轮验证强支持此前猜想：当前 false SafeIn 偏高的主因不是简单的 nprobe 增大，也不只是全局 `safein_d_k` 数值偏大，而是全局静态阈值 `T` 与 query-level top-k 半径 `R_q` 的异质性错配。

在线上 `bench_vector_search` 真实路径下，false SafeIn 几乎全部集中在 `T > R_q` 的 easy query：

| p | T | total SafeIn | false SafeIn | false/total | `T>R_q` queries | `T>R_q` false share | `T>R_q` false rate | `T<=R_q` false rate |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 0.90 | 1.30685 | 2,483 | 1,010 | 40.68% | 100 | 99.50% | 53.03% | 0.85% |
| 0.95 | 1.29626 | 1,428 | 483 | 33.82% | 50 | 99.59% | 51.89% | 0.40% |
| 0.97 | 1.28587 | 843 | 259 | 30.72% | 29 | 99.61% | 51.29% | 0.29% |

这说明 `safein_dk_percentile` 调低确实能减少 total SafeIn 和 false SafeIn，但它没有消除结构性问题：只要存在 `T > R_q` 的 query，这部分 query 的 false rate 仍然约 51%-53%。

## 验证口径

- 数据集：COCO100k。
- Index：`/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90`。
- Base/query：`/home/zcq/VDB/data/coco_100k/image_embeddings.fvecs` 与 `query_embeddings.fvecs`。
- `image_ids`：`/home/zcq/VDB/data/coco_100k/image_ids.npy`。
- `nlist=2048`、`nprobe=64`、`topk=10`、`bits=4`、`queries=1000`。
- `bench_vector_search` 使用真实线上两阶段路径，新增 `--per-query-stats-output` 仅输出每个 query 的统计，不改变查询逻辑。
- `R_q` 来自 brute-force exact L2 top-k 半径。该口径与当前 exact `safein_d_k` calibration 一致。

## 线上 Per-query 分解

### p=0.90

| group | queries | queries with SafeIn | queries with false | SafeIn | false | false rate | SafeIn share | false share |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `T > R_q` | 100 | 100 | 63 | 1,895 | 1,005 | 53.03% | 76.32% | 99.50% |
| `T <= R_q` | 900 | 276 | 5 | 588 | 5 | 0.85% | 23.68% | 0.50% |

Top false queries 都是低半径 easy query：

| query | R_q | T-R_q | SafeIn | false | false rate |
|---:|---:|---:|---:|---:|---:|
| 328 | 1.25000 | 0.05685 | 84 | 74 | 88.10% |
| 20 | 1.22129 | 0.08556 | 82 | 72 | 87.80% |
| 136 | 1.22595 | 0.08090 | 73 | 63 | 86.30% |
| 602 | 1.25509 | 0.05176 | 62 | 52 | 83.87% |
| 320 | 1.28521 | 0.02164 | 54 | 44 | 81.48% |

### p=0.95

| group | queries | queries with SafeIn | queries with false | SafeIn | false | false rate | SafeIn share | false share |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `T > R_q` | 50 | 50 | 38 | 927 | 481 | 51.89% | 64.92% | 99.59% |
| `T <= R_q` | 950 | 240 | 2 | 501 | 2 | 0.40% | 35.08% | 0.41% |

### p=0.97

| group | queries | queries with SafeIn | queries with false | SafeIn | false | false rate | SafeIn share | false share |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `T > R_q` | 29 | 29 | 19 | 503 | 258 | 51.29% | 59.67% | 99.61% |
| `T <= R_q` | 971 | 175 | 1 | 340 | 1 | 0.29% | 40.33% | 0.39% |

## Gap 分箱观察

`T-R_q` 越接近或超过 0，SafeIn 和 false 都快速集中。p=0.90 下：

| gap bin | queries | SafeIn | false | false rate | false share |
|---|---:|---:|---:|---:|---:|
| `(-inf, -0.10]` | 93 | 4 | 0 | 0.00% | 0.00% |
| `(-0.10, -0.05]` | 394 | 56 | 0 | 0.00% | 0.00% |
| `(-0.05, -0.02]` | 296 | 182 | 0 | 0.00% | 0.00% |
| `(-0.02, 0]` | 117 | 346 | 5 | 1.45% | 0.50% |
| `(0, inf)` | 100 | 1,895 | 1,005 | 53.03% | 99.50% |

这说明 false SafeIn 的边界非常清晰：一旦 `T` 高于该 query 的 exact top-k 半径，false 会被候选数量放大；当 `T <= R_q` 时，线上 false 几乎消失。

## 候选级 False Mode 分解

为了区分 false 是来自静态阈值还是误差界失效，对 p=0.90 额外运行了 `bench_dk_space_compare --write-candidates 1` 的离线 S2 replay。该工具遍历 nprobe 候选并输出：

```text
U_i = d_hat_i + e_i
static SafeIn iff U_i < T
oracle SafeIn iff U_i < R_q
```

候选级结果：

| replay | SafeIn | false | false rate |
|---|---:|---:|---:|
| static `T` | 135 | 73 | 54.07% |
| oracle `R_q` | 166 | 0 | 0.00% |

static `T` 的 73 个 false 全部属于：

```text
static_T_above_query_Rq
```

即 `R_q < d_i < T`，没有观察到 `U_i < T < d_i` 这类误差界失效。在这个离线 S2 replay 中，per-query oracle `T_q=R_q` 不但消除了 false，还因为 hard query 的阈值更大，SafeIn 从 135 增加到 166。

注意：这个候选级 replay 不是线上数量复现。它使用全 S2 离线 replay，不包含完整 Stage1/FastScan 与在线 scheduling 的所有细节；因此只用于解释 false 来源，不用于替代线上 `bench_vector_search` 数量。

## 生成的原始文件

- `validation_summary.json`：线上 per-query 汇总与 p=0.90 候选级分解。
- `query_gap_bins.csv`：按 `T-R_q` 分箱的 SafeIn/false 统计。
- `top_false_queries.csv`：每个 p 下 false 最多的 query。
- `p090_candidate_false_modes.json`：p=0.90 候选级 false mode 汇总。
- `validation_p090/online_per_query.csv`、`validation_p095/online_per_query.csv`、`validation_p097/online_per_query.csv`：线上 per-query 原始统计。
- `validation_p090/dk_space_compare_candidates_exact.csv`：p=0.90 候选级离线 replay 原始 CSV。
- 每个 validation 子目录下的 `results.json` 和 `run.log`：benchmark 原始输出。

## 研究判断

本轮结果说明：

1. 全局静态 `T` 的主要缺陷是 query-agnostic，而不是单纯 percentile 选错。
2. 降低 `T` 会减少触发 `T>R_q` 的 query 数量，因此 false 总量下降，但剩余 `T>R_q` query 的 false rate 仍维持在约 51%-53%。
3. 对 `T<=R_q` 的 query，线上 falseSafeIn 基本可以忽略，说明当前大部分 false 不是普遍的误差界崩坏。
4. 后续如果要同时提高 SafeIn 覆盖和 purity，应转向 query-adaptive threshold，而不是继续只调全局 `safein_dk_percentile`。

## 建议下一步

优先做一个 query-adaptive replay：

```text
T_q = conservative_estimate(R_q)
SafeIn iff U_i < T_q
```

候选 `T_q` 可以来自：

- 当前 probe 内的 `kth(U)` 或其保守下界。
- coarse score gap / query-to-centroid distance / probed cluster rank 的回归或分桶校准。
- exact `R_q` oracle replay 作为上界参考，用于判断自适应方法最多能提升多少。

评价指标应继续使用线上 per-query 输出：

- total SafeIn。
- falseSafeIn / SafeIn。
- SafeIn 在 `T>R_q` 与 `T<=R_q` query 上的分布。
- recall@10 与 latency。

