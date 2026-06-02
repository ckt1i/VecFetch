# Dynamic SafeIn d_k 实验报告

## 实验设置

- 数据集：COCO100k，1000 个 query。
- Index：`/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90`。
- 参数：`nlist=2048`、`nprobe=64`、`topk=10`、`bits=4`、`metric=cosine`。
- Benchmark：`build/benchmarks/bench_vector_search`，生产 `OverlapScheduler` 路径。
- 输出目录：`safeIn_dk_review_logs/dynamic_prefetch_runs/`。
- SafeIn d_k 样本：`dynamic_prefetch_runs/safein_exact_dk_samples.npy`。

本轮测试了静态 p=0.90/0.95/0.97、upper frontier cap/scale、lower frontier delay/stable，以及 payload-only gate 和分类阈值动态化两种接入方式。

## 结果总表

相对变化均以 `static_p090` 为基线。

| scheme | recall@10 | avg ms | false SafeIn | false rate | all reads | remaining payload | 判断 |
|---|---:|---:|---:|---:|---:|---:|---|
| `static_p090` | 0.9455 | 1.770 | 982 | 40.8% | 2408 | 8566 | 最快，但 false 高 |
| `static_p095` | 0.9454 | 1.857 | 468 | 33.8% | 1384 | 9076 | 简单稳健 baseline |
| `static_p097` | 0.9454 | 2.625 | 241 | 29.8% | 810 | 9426 | purity 提升但延迟高 |
| `upper_cap_p090` | 0.9455 | 2.397 | 661 | 31.8% | 2080 | 8573 | false 降低有限，延迟偏高 |
| `upper_scale094_payload_p090` | 0.9455 | 2.610 | 982 | 40.8% | 143 | 9863 | payload 预取大幅减少但延迟差 |
| `upper_scale092_payload_p090` | 0.9455 | 2.445 | 982 | 40.8% | 69 | 9932 | payload 预取最少但延迟差 |
| `lower_delay_payload_p090` | 0.9455 | 2.486 | 982 | 40.8% | 937 | 9188 | 只省 payload，不降 false |
| `lower_stable_payload_p090` | 0.9455 | 2.146 | 982 | 40.8% | 373 | 9642 | payload-only 中最好，但仍慢 |
| `lower_delay_classify_p090` | 0.9453 | 1.987 | 136 | 14.0% | 971 | 9162 | 最佳综合方案 |
| `lower_stable_classify_p090` | 0.9453 | 2.501 | 15 | 3.9% | 384 | 9631 | 最高 purity 方案 |

## 推荐方案

### 1. 首选：`lower_delay_classify_p090`

配置：

```text
--safein-dk-percentile 0.90
--dynamic-safein frontier_delay
```

效果：

- false SafeIn：982 -> 136，降低 86.2%。
- false rate：40.8% -> 14.0%。
- `all_read_requests`：2408 -> 971，降低 59.7%。
- recall@10：0.9455 -> 0.9453，仅 -0.0002。
- avg latency：1.770 ms -> 1.987 ms，增加 12.3%。

这是当前最平衡的方案：明显压低 false SafeIn 和 payload 预取，同时 recall 基本不动，延迟代价可控。

### 2. 高 purity 备选：`lower_stable_classify_p090`

配置：

```text
--safein-dk-percentile 0.90
--dynamic-safein frontier_stable
--dynamic-safein-stable-probes 2
--dynamic-safein-rel-tol 0.002
```

效果：

- false SafeIn：982 -> 15，降低 98.5%。
- false rate：40.8% -> 3.9%。
- `all_read_requests`：2408 -> 384，降低 84.1%。
- recall@10：0.9455 -> 0.9453，仅 -0.0002。
- avg latency：1.770 ms -> 2.501 ms，增加 41.3%。

如果目标是尽量证明 SafeIn purity，可选它；如果目标是 serving latency，不建议作为默认。

### 3. 简单保守 baseline：`static_p095`

配置：

```text
--safein-dk-percentile 0.95
--dynamic-safein static
```

效果：

- false SafeIn 降低 52.3%。
- avg latency 只增加 4.9%。
- 不引入查询期间动态逻辑。

它不是动态方案，但可作为上线前的低风险对照。

## 不推荐作为主方案

- Payload-only gate 虽然能显著减少 `all_read_requests`，但 false SafeIn 统计不变，且 final payload fetch 增加，实际 avg latency 全部变差。
- `upper_cap_p090` 说明 `kth(d_hat+e)` cap 可以减少部分 false，但它不是 SafeIn purity 的好代理，延迟也偏高。
- `upper_scale` 能把 payload 预取压到很低，但主要把 I/O 延后到 final payload fetch，端到端收益不好。

## 后续可继续调参

最值得继续细扫的是 lower-bound classify：

- `frontier_delay` 的 `--dynamic-safein-min-probes`：测试 2/4/8，避免过早启用。
- `frontier_stable` 的 `--dynamic-safein-rel-tol`：测试 0.001/0.005/0.01，寻找 purity 和延迟平衡点。
- 对 `lower_delay_classify_p090` 做 3 次重复，确认 12% latency 代价是否稳定。

当前建议默认从 `lower_delay_classify_p090` 开始。
