# SE 低 SafeIn Credit 权重结果

日期：2026-07-18

## 结论

在 `topk=100,nprobe=96`、传统 SafeIn full-record prefetch 关闭、
`alpha=3/2,tail=0` 的同口径五次配对中，`rho=0.1` 在 ESCI 和 MSMARCO 上
都比 `rho=0.25` 具有更小的 paired-median QPS 回退，同时仍保持严格的请求数
下降。因此后续 NoCombine 与 SafeIn-prefetch on/off 消融采用 `SE,rho=0.1`；
`rho=0.25` 保留为请求收益更强的 Pareto 备选。

## 五次配对结果

| rho | 数据集 | QPS delta | QPS mean±std | p99 delta | request delta | byte delta | planner ms delta |
|---:|---|---:|---:|---:|---:|---:|---:|
| 0.25 | ESCI | -0.50% | -0.43%±0.23% | +1.23% | -1.37% | +0.90% | +0.0026 |
| 0.25 | MSMARCO | -0.33% | -0.18%±0.28% | +0.81% | -0.72% | +0.32% | +0.0015 |
| 0.10 | ESCI | -0.40% | -0.12%±0.50% | +0.20% | -0.69% | +0.46% | +0.0022 |
| 0.10 | MSMARCO | -0.27% | -0.24%±0.37% | +0.75% | -0.22% | +0.10% | +0.0011 |

QPS 为逐 repetition 的 `SE/GE-1` 后取中位数；request/byte 为相同口径。
五次运行的 recall、probed、reranked、unique-fetch coverage 完全一致，所有
planner fallback 为 0，planned physical bytes 与 issued vector bytes 完全一致。

## 原始 QPS delta

- `rho=0.25,ESCI`：`[-0.1260,-0.6879,-0.2710,-0.4976,-0.5635]%`。
- `rho=0.25,MSMARCO`：`[+0.0501,+0.1880,-0.3303,-0.3774,-0.4516]%`。
- `rho=0.10,ESCI`：`[+0.6223,-0.4029,-0.5418,-0.4510,+0.1509]%`。
- `rho=0.10,MSMARCO`：`[-0.2699,+0.1969,-0.7546,+0.0164,-0.3857]%`。

## 解释与下一步

降低 `rho` 单调减少 planner 可计入的 credit，因此请求减少和附加读取字节均下降；
端到端 QPS 受运行噪声影响，不严格单调，但 `rho=0.1` 在两个数据集的中位数上
均优于 `rho=0.25`。后续先测试传统 SafeIn prefetch on/off；正式判断使用同-rep
配对结果。若第一轮不足两个同一 `nprobe` 的有效数据集，再附加“最低
NoSafeIn QPS 固定基线”口径，但不覆盖正式配对表。

原始结果：

- `/home/zcq/VDB/test/recordgate_span_se_low_rho_sweep_20260718`
- `/home/zcq/VDB/test/recordgate_span_se_low_rho_smoke_20260718`
