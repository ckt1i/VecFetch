# Span 四方案与 Exact Secondary Screen 结果分析

日期：2026-07-18

## 结论

最终选择 **GE（vector-only exact）** 作为
`theory-preferred, empirically non-inferior default`。GV 仍是绝对时延最低、
实现最简单的基线；SafeIn-aware SV/SE 保留为补充消融，不进入默认 span 分组。

三数据集 `nprobe=192` secondary screen 中 GE 与 SE 都曾达到用户放宽后的
2/3 非劣门禁；但新增的五次 `nprobe=96` 主实验给出了区分：GE-GV 在
ESCI/MSMARCO 分别为 -0.89%/-0.62% QPS，均通过；SE-SV 则为
-1.22%/-1.07%，均越过 -1% 门禁。即使把 SafeIn 权重降为 `rho=1/2`，
SE 相对 GV 仍为 -1.30%/-1.20%。因此最终采用“exact=yes、SafeIn credit=no”的
GE，而不是初步筛选阶段的 SE。

## 五次 q500 配对结果

下表为逐 repetition 计算 delta 后的中位数；QPS 正值更好，其余列负值更好。

| 数据集 | 对比 | QPS delta | p99 delta | request delta | byte delta |
|---|---|---:|---:|---:|---:|
| Amazon ESCI | GE-GV | -0.77% | +0.39% | -1.34% | +0.81% |
| MSMARCO | GE-GV | -0.70% | +0.69% | -0.22% | +0.10% |
| VoxCeleb2 | GE-GV | -2.20% | +0.60% | -3.52% | +1.66% |
| Amazon ESCI | SE-SV | -0.78% | +0.81% | -2.61% | +1.75% |
| MSMARCO | SE-SV | -0.80% | +0.31% | -1.04% | +0.52% |
| VoxCeleb2 | SE-SV | -2.28% | -0.01% | -3.52% | +1.66% |
| Amazon ESCI | SE-GE | -0.56% | +0.45% | -3.53% | +2.63% |
| MSMARCO | SE-GE | -0.55% | +0.04% | -2.16% | +1.07% |
| VoxCeleb2 | SE-GE | -0.21% | -0.13% | 0.00% | 0.00% |

冻结门禁为：paired QPS `>=-1%`、p99 `<=+3%`、字节 `<=+5%`、请求数不高于
matched greedy、语义一致且无 fallback。

- GE-GV：ESCI、MSMARCO 通过，Vox 失败，合计 2/3。
- SE-SV：ESCI、MSMARCO 通过，Vox 失败，合计 2/3。
- SE-GE：三个数据集全部通过；ESCI/MSMARCO 的 SafeIn credit 能继续减少请求。

## 绝对中位指标

| 数据集 | 模式 | avg query ms | planner ms | requests | bytes | credit bytes |
|---|---|---:|---:|---:|---:|---:|
| ESCI | GV | 5.183 | 0.0225 | 334.74 | 2,552,040.82 | 0 |
| ESCI | GE | 5.225 | 0.0558 | 330.26 | 2,572,770.26 | 0 |
| ESCI | SV | 5.197 | 0.0231 | 327.16 | 2,595,138.58 | 82,530.30 |
| ESCI | SE | 5.243 | 0.0576 | 318.61 | 2,640,506.62 | 85,741.57 |
| MSMARCO | GV | 5.849 | 0.0239 | 336.83 | 3,906,221.44 | 0 |
| MSMARCO | GE | 5.896 | 0.0645 | 336.10 | 3,909,991.30 | 0 |
| MSMARCO | SV | 5.876 | 0.0248 | 332.30 | 3,931,409.92 | 40,118.27 |
| MSMARCO | SE | 5.924 | 0.0658 | 328.85 | 3,951,753.15 | 42,274.82 |
| Vox | GV | 3.206 | 0.0109 | 147.21 | 314,353.68 | 0 |
| Vox | GE | 3.286 | 0.0429 | 142.03 | 319,580.06 | 0 |
| Vox | SV | 3.210 | 0.0111 | 147.21 | 314,353.68 | 0 |
| Vox | SE | 3.290 | 0.0431 | 142.03 | 319,580.06 | 0 |

Vox 的 inline store 在当前访问下没有可计入的内部 SafeIn payload，因此
`SV==GV`、`SE==GE` 的分组与字节完全相同。该边界结果证明 SafeIn credit 只来自
实际可复用的 inline bytes，不会把外部/cold payload 虚构成有效读取量。

## nprobe=96 主实验与 rho=1/2 confirmation

五次 q500、两个核心数据集的 `nprobe=96` factorial contrasts：

| 数据集 | 对比 | QPS | p99 | requests | bytes |
|---|---|---:|---:|---:|---:|
| ESCI | GE-GV | -0.89% | +0.12% | -1.49% | +0.87% |
| MSMARCO | GE-GV | -0.62% | +0.21% | -0.22% | +0.10% |
| ESCI | SE-SV | -1.22% | +0.85% | -2.87% | +1.88% |
| MSMARCO | SE-SV | -1.07% | +0.53% | -1.06% | +0.52% |

三次 `rho=1/2` confirmation 表明降低 SafeIn 权重能改善 SE-GE，但不能让
SafeIn-aware 方案成为稳定默认：

| 数据集 | 对比 | QPS | p99 | requests | bytes |
|---|---|---:|---:|---:|---:|
| ESCI | SV-GV | -0.16% | +0.20% | -1.42% | +1.00% |
| MSMARCO | SV-GV | -0.03% | +0.21% | -0.69% | +0.31% |
| ESCI | SE-GE | -0.26% | +1.47% | -2.23% | +1.52% |
| MSMARCO | SE-GE | -0.24% | +0.16% | -1.21% | +0.56% |
| ESCI | SE-SV | -1.14% | +0.93% | -2.30% | +1.39% |
| MSMARCO | SE-SV | -0.59% | +0.65% | -0.74% | +0.34% |

## Current-binary NoSpan anchor

GE 决策前先确认基础 span 机制在新 binary 上仍成立。NoSpan 与 GV 的中位结果：

| nprobe | 数据集 | GV QPS delta | request delta | byte delta |
|---:|---|---:|---:|---:|
| 96 | ESCI | +1.82% | -55.20% | +16.23% |
| 96 | MSMARCO | +4.81% | -68.05% | +23.44% |
| 192 | ESCI | +0.91% | -53.46% | +15.50% |
| 192 | MSMARCO | +4.60% | -67.36% | +23.20% |

因此此前“适度读放大换取显著请求合并”的因果链在 current binary 上复现，
不是旧二进制遗留结果。

## Representative ablation drift checks

GE、`alpha=3/2`、`tail=0` 下，Combined 相对 NoCombine 的三次配对中位
QPS 为：ESCI +2.10%、COCO +9.58%、MSMARCO +10.81%、Vox +6.12%；
四点 recall 全部一致。现有 layout/NoCombine 结论方向未反转，可以复用旧完整
sweep，并用本轮四个 representative points 做 current-binary drift anchor。

FullCurrent 相对两个 pipeline comparator 的三次配对中位 QPS：

| 数据集 | Full-OldNoOverlap | Full-NewNoPipeline |
|---|---:|---:|
| COCO | +2.73% | +2.70% |
| MSMARCO | +7.54% | +7.50% |
| Vox | +135.41% | +135.54% |

三数据集 recall 全部一致，pipeline 结论方向也未反转。Vox 的大幅值应按该
serial-no-overlap workload 的边界结果陈述，不宜外推成所有配置的通用收益。

## 正确性与实验边界

- 五个 repetitions、三个数据集的 recall、probed、reranked 在四模式间完全一致。
- 所有结果 `fallback=0`。
- 每个结果均满足 planned physical bytes = issued vector bytes，planner groups =
  issued vector requests。
- 当前数学模型与实现均为 tail-free：`vec_span_safein_tail_count=0`。旧 endpoint
  tail 不能与新的 rational planner 结果直接叠加。

## 论文可支持的表述

可以写：在受控读放大下，vector-only endpoint-dominance exact planner 严格
优化请求优先、字节次优的分组目标。其 CPU 代价在核心数据集上约
0.06 ms/query；在 `nprobe={96,192}` 的四个核心 cell 中均满足相对 GV 的
QPS/p99/字节非劣门禁，并严格减少或保持请求数。

SafeIn-aware credit 可以写成补充机制：存在真实 inline reuse 时能进一步减少请求，
但 `rho=1` 在 nprobe=96 不稳定，`rho=1/2` 仍未形成相对 GV 的默认优势。
不应写 SE 是 QPS winner，或 SafeIn credit 对所有 layout 都有效。

原始结果：

- `/home/zcq/VDB/test/recordgate_span_four_way_cpu_round3_20260718`
- `/home/zcq/VDB/test/recordgate_span_exact_secondary_screen_20260718`
- `/home/zcq/VDB/test/recordgate_span_four_way_formal_20260718`
- `/home/zcq/VDB/test/recordgate_span_se_rho_half_confirmation_20260718`
- `/home/zcq/VDB/test/recordgate_span_nospan_anchor_20260718`
- `/home/zcq/VDB/test/recordgate_span_ge_nocombine_drift_20260718`
- `/home/zcq/VDB/test/recordgate_span_ge_pipeline_drift_20260718`
