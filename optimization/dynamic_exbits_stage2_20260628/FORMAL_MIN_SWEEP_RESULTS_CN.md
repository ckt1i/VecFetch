# 最小 Formal Sweep 结果：Stage2 格式对比

日期：2026-06-28

## 实验口径

- 数据集：Amazon ESCI、MSMARCO Passage
- layout：
  - official-like baseline：`vector_bitplanes`
  - 新格式：`tile_lane_bitmajor`
- `stored_ex_bits=3`
- `active_ex_bits=3`
- `topk=10`
- `nprobe=64,128,256,512`
- `queries=1000`
- two-level coarse routing：开启，`budget_factor=16`
- `non_safeout_candidate_budget=400`
- progressive active bits：关闭
- 索引：复用 `/home/zcq/VDB/test/dynamic_exbits_stage2_20260628/indexes`
- 结果：`/home/zcq/VDB/test/dynamic_exbits_stage2_20260628/runs/stage2_format_formal_q1000_nprobe_sweep`

## 结果

### Amazon ESCI

| nprobe | vector avg ms | tile avg ms | total speedup | vector Stage2 ms | tile Stage2 ms | Stage2 speedup | recall@10 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 0.5117 | 0.5142 | -0.49% | 0.0537 | 0.0508 | 5.63% | 0.7668 |
| 128 | 0.7291 | 0.7307 | -0.22% | 0.0509 | 0.0474 | 7.34% | 0.8550 |
| 256 | 1.3841 | 1.3792 | 0.35% | 0.0557 | 0.0546 | 2.04% | 0.8893 |
| 512 | 2.8416 | 2.8720 | -1.06% | 0.0615 | 0.0604 | 1.91% | 0.9049 |

### MSMARCO Passage

| nprobe | vector avg ms | tile avg ms | total speedup | vector Stage2 ms | tile Stage2 ms | Stage2 speedup | recall@10 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 1.2660 | 1.2524 | 1.09% | 0.1595 | 0.1526 | 4.54% | 0.8101 |
| 128 | 1.8101 | 1.8305 | -1.11% | 0.1599 | 0.1533 | 4.29% | 0.8681 |
| 256 | 2.9588 | 2.9652 | -0.22% | 0.1606 | 0.1531 | 4.86% | 0.8950 |
| 512 | 5.0141 | 5.0514 | -0.74% | 0.1609 | 0.1539 | 4.53% | 0.9104 |

## 结论

`tile_lane_bitmajor` 的 Stage2 kernel 收益是稳定存在的：

- Amazon：约 1.9%-7.3% Stage2 加速；
- MSMARCO：约 4.3%-4.9% Stage2 加速。

但总查询时间没有稳定达到 2% 加速：

- Amazon 总时间在 -1.06% 到 +0.35% 之间；
- MSMARCO 最好点为 +1.09%，其余点略慢。

原因是当前 workload 中 Stage2 只占总查询的一小部分，coarse routing、Stage1、submit/I/O 调度占比更大。单靠 Stage2 内存布局，即使 Stage2 快 5%左右，也不足以转化为 2% 以上的端到端收益。

因此当前证据支持的表述应更谨慎：

- 可以声称新格式降低 Stage2 计算成本，并支持 `stored_ex_bits > active_ex_bits` 的查询时动态降级；
- 不应声称当前实现已经稳定带来 2% 以上端到端加速；
- 若论文贡献要强调格式性能，应将指标聚焦到 Stage2 或量化码扫描成本，并把端到端结果作为系统内综合效果，而不是唯一证据。

## 补充：topk=100

运行口径同上，但 `topk=100`。

### Amazon ESCI

| nprobe | vector avg ms | tile avg ms | total speedup | vector Stage2 ms | tile Stage2 ms | Stage2 speedup | recall@100 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 0.8497 | 0.8577 | -0.94% | 0.1656 | 0.1643 | 0.78% | 0.7663 |
| 128 | 1.1412 | 1.1657 | -2.10% | 0.1822 | 0.1846 | -1.29% | 0.8580 |
| 256 | 1.8991 | 1.9211 | -1.14% | 0.2148 | 0.2194 | -2.10% | 0.9229 |
| 512 | 3.4649 | 3.5144 | -1.41% | 0.2550 | 0.2660 | -4.11% | 0.9590 |

### MSMARCO Passage

| nprobe | vector avg ms | tile avg ms | total speedup | vector Stage2 ms | tile Stage2 ms | Stage2 speedup | recall@100 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 64 | 1.5799 | 1.6035 | -1.47% | 0.2916 | 0.2955 | -1.31% | 0.8082 |
| 128 | 2.1639 | 2.2057 | -1.90% | 0.2989 | 0.3119 | -4.16% | 0.8772 |
| 256 | 3.3243 | 3.3839 | -1.76% | 0.3025 | 0.3078 | -1.72% | 0.9166 |
| 512 | 5.4244 | 5.5132 | -1.61% | 0.3061 | 0.3104 | -1.40% | 0.9394 |

`topk=100` 下 `tile_lane_bitmajor` 不再显示 Stage2 收益，端到端也整体更慢。该结果进一步说明：新格式不适合作为“通用端到端加速”claim；更适合限定在 `topk=10` 或 Stage2 code-scan 组件成本。

## 补充：active_ex_bits=1/2/3

运行口径：

- layout：`tile_lane_bitmajor`
- `stored_ex_bits=3`
- `topk=10`
- `nprobe=64`
- `queries=1000`

### Amazon ESCI

| active_ex_bits | avg ms | QPS | recall@10 | Stage2 ms | Stage2 lanes |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 0.4955 | 2018.31 | 0.6797 | 0.0371 | 274.814 |
| 2 | 0.4974 | 2010.55 | 0.7161 | 0.0433 | 270.314 |
| 3 | 0.4849 | 2062.43 | 0.7668 | 0.0478 | 286.400 |

### MSMARCO Passage

| active_ex_bits | avg ms | QPS | recall@10 | Stage2 ms | Stage2 lanes |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 1 | 1.2203 | 819.48 | 0.7051 | 0.1283 | 729.017 |
| 2 | 1.2275 | 814.65 | 0.7220 | 0.1318 | 729.571 |
| 3 | 1.2545 | 797.12 | 0.8101 | 0.1523 | 750.986 |

该结果可以支撑“同一高 bit 索引支持查询期 active bit 降级，形成速度/精度折中”的表述，尤其 MSMARCO 上趋势清晰。Amazon 上端到端受其他阶段噪声影响，`active=3` 反而总时间略快，但 Stage2 时间仍随 active bits 增加。

## 补充：nprobe=64, topk=10, REPS=5

| dataset | layout | avg ms mean | avg ms std | Stage2 ms mean | Stage2 ms std | recall@10 |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Amazon ESCI | vector_bitplanes | 0.5134 | 0.0013 | 0.0539 | 0.0001 | 0.7668 |
| Amazon ESCI | tile_lane_bitmajor | 0.5065 | 0.0098 | 0.0501 | 0.0012 | 0.7668 |
| MSMARCO Passage | vector_bitplanes | 1.2597 | 0.0028 | 0.1596 | 0.0002 | 0.8101 |
| MSMARCO Passage | tile_lane_bitmajor | 1.2540 | 0.0021 | 0.1525 | 0.0005 | 0.8101 |

REPS=5 后：

- Amazon：端到端 +1.35%，Stage2 +7.55%；
- MSMARCO：端到端 +0.46%，Stage2 +4.71%。

该多次运行结果确认 Stage2 收益稳定，但仍不足以支撑 2% 端到端目标。
