# Aggressive Dynamic SafeIn 实验报告

> Historical note: 本报告记录探索阶段结果，其中 `frontier_blend`、
> scale/lambda 和 lower delay/stable 命令已经不再是受支持 CLI。当前正式
> 实现已迁移为 `--dynamic-safein frontier`，语义等价于后续补跑的
> `blend000_defer4` / `T_q=F_lower`。

## 目标

本轮目标改为 prefetch 口径：

```text
safein_prefetch_true_topk / (queries * topk) > 20%
safein_prefetch_false / safein_prefetch_candidates < 20%
```

因此本轮新增了 `safein_prefetch_*` 指标，直接统计实际发出
`VEC_ALL` 的 SafeIn prefetch，而不是只看 prober 内部
SafeIn/Uncertain/SafeOut 分类。

## 新增方案

- `frontier_blend`：动态阈值使用 `lower + lambda * (upper - lower)`，
  解决 lower frontier 太保守、upper frontier false 太高的问题。
- warmup defer：前若干聚类先缓存候选的地址和估计上界，等 query-level
  frontier 形成后统一回补重判 SafeIn，再决定 `VEC_ALL` / `VEC_ONLY`。
- `--skip-false-stats`：用于 MSMARCO 这种大规模 index，跳过从 index
  payload 恢复全量 cluster members 的慢路径；COCO 精确实验不使用该开关。

## COCO100k 结果

完整输出：

- `aggressive_dynamic_prefetch_runs/summary.csv`
- `aggressive_dynamic_prefetch_runs/summary.json`
- 每个 run 的 `cmd.txt`、`run.log`、`results.json`、`online_per_query.csv`

关键结果如下：

| scheme | topk | target | coverage | false rate | recall@k | avg ms | all reads | final payload |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| `static_p090` | 10 | no | 14.26% | 40.78% | 0.9455 | 1.845 | 2408 | 8566 |
| `lower_delay_defer4_p090` | 10 | no | 10.51% | 1.13% | 0.9453 | 1.741 | 1063 | 8949 |
| `blend005_defer4_p090` | 10 | yes | 49.07% | 12.47% | 0.9457 | 2.563 | 5606 | 4977 |
| `blend010_defer4_p090` | 10 | yes | 51.27% | 13.32% | 0.9457 | 2.379 | 5915 | 4745 |
| `blend020_defer4_p090` | 10 | yes | 55.70% | 14.78% | 0.9457 | 2.617 | 6536 | 4281 |
| `static_p090` | 50 | no | 17.82% | 32.78% | 0.9356 | 2.436 | 13254 | 41055 |
| `lower_delay_defer4_p085` | 50 | near | 19.59% | 3.43% | 0.9356 | 3.057 | 10145 | 40189 |
| `blend002_defer4_p090` | 50 | no | 62.01% | 22.79% | 0.9362 | 2.571 | 40155 | 17838 |

topk=10 已经明确达标；topk=50 的 best conservative 方案接近 coverage
目标但未过 20%，blend 系列 coverage 高但 false rate 超过 20%。

## COCO100k bench_e2e

输出：

- `aggressive_e2e_coco/summary.csv`
- `aggressive_e2e_coco/summary.json`

`bench_e2e` 复用同一 index，`topk=10`，`nprobe=64`，resident/full-preload
cluster 模式。`bench_e2e` 目前复用单个 scheduler，因此没有逐 query 传入
GT top-k set；这里比较 recall/latency/read 行为，精确 false/coverage 以
`bench_vector_search` 为准。

| scheme | recall@10 | avg ms | p95 ms | all reads/query | final payload/query |
|---|---:|---:|---:|---:|---:|
| `static_p090` | 0.9455 | 0.798 | 1.037 | 2.4 | 8.6 |
| `blend005_defer4_p090` | 0.9457 | 1.068 | 1.362 | 5.6 | 5.0 |
| `blend010_defer4_p090` | 0.9457 | 0.745 | 0.971 | 5.9 | 4.7 |
| `blend020_defer4_p090` | 0.9457 | 0.976 | 1.267 | 6.5 | 4.3 |

端到端最好的候选是 `blend010_defer4_p090`：recall 略高于 static，
final payload fetch 从 8.6/query 降到 4.7/query，同时 avg/p95 latency
也优于本轮 static run。

## MSMARCO 验证

准备产物：

- adapter：`msmarco_passage_adapter_for_aggressive_safein/`
- 复用 index：`/home/zcq/VDB/test/data/MSMACRO/msmarco_passage_adapter_full_20260424T191013/index`
- 结果：`aggressive_vector_msmarco_200_skip_false/summary.csv`

尝试直接在 MSMARCO 上恢复 false stats 时，`RecoverClusterMembersFromIndex`
需要扫描/恢复 8.8M 向量的 cluster members，两次进入长时间 D state。
因此 MSMARCO 验证使用 `--skip-false-stats 1`，只比较 recall、latency 和
读取行为；不声称 MSMARCO 上已经证明 false rate。

| scheme | recall@10 | avg ms | p95 ms | all reads | vec-only reads | final payload |
|---|---:|---:|---:|---:|---:|---:|
| `static_loaded_dk` cold | 0.9425 | 505.688 | 1145.500 | 19 | 147968 | 1981 |
| `static_warm_loaded_dk` | 0.9425 | 64.098 | 75.950 | 19 | 147968 | 1981 |
| `blend010_defer4_loaded_dk` | 0.9430 | 70.486 | 81.923 | 1852 | 146302 | 615 |

MSMARCO 上 `blend010` 保持 recall，并把 final payload fetch 从 1981 降到
615，但 warm latency 比 static 高约 6.4ms。也就是说，该方案的大规模读取
行为有效，但 MSMARCO 端到端 latency 没有优于 warm static。

## 推荐

1. 当前正式候选：`frontier_defer4_p090`
   - 迁移自 `blend000_defer4`，COCO coverage 47.41%，false rate 11.53%，满足目标且 purity 优于 `blend010`。
   - 配置：
     ```text
     --dynamic-safein frontier
     --dynamic-safein-stable-probes 1
     --dynamic-safein-defer-initial-clusters 4
     --dynamic-safein-defer-until-ready 1
     ```

2. 高 coverage 备选：`topk10_blend020_defer4_p090`
   - COCO coverage 55.70%，false rate 14.78%。
   - e2e latency 比 `blend010` 差，不建议作为默认。

3. 高 purity 备选：`topk50_lower_delay_defer4_p085`
   - COCO topk=50 coverage 19.59%，false rate 3.43%，接近目标。
   - 如果后续主要服务 topk=50，可以继续扫 p=0.84/0.83 或增加 defer
     cluster 数，争取把 coverage 推过 20%。

当前正式实现建议以 `frontier_defer4_p090` 作为默认方案；本报告中的
`frontier_blend` / scale 结果仅作为 historical 探索依据保留。
