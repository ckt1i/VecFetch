# SE/Combine 与 NoCombine 消融结果

日期：2026-07-18

## 结论

采用低权重胜出配置 `SE,rho=0.1,alpha=3/2,tail=0` 后，CombinedSE 相对
NoCombine 在四个数据集上均取得稳定的端到端收益：五次同-repetition 配对的 QPS
中位数分别提升 `+0.94%/+6.26%/+9.06%/+9.20%`，每个数据集均为 `5/5`
次正向；p99 中位数也全部下降。该结果足以将 NoCombine 作为论文中的系统组件
消融。

但是，这不是严格的“只切换物理布局、保持规划器不变”实验。SafeIn-aware SE
要求 inline record、resident record metadata 与 payload reuse，不能在 separate store
上合法运行。因此本实验比较的是完整系统配置：

- `CombinedSE`：合并 record 布局，`SE,rho=1/10`，payload reuse 开启；
- `NoCombine`：向量与 payload 分离，`GE,rho=0`，payload reuse 关闭；
- 两组均关闭传统 eager SafeIn prefetch，使用 late materialization，并保持
  `safein_as_vec_only=true`。

论文应将其表述为“关闭 record combining 及其所启用的 payload-aware cofetch
能力”的组件消融，不能据此单独宣称 SE 一定优于 GE。

## 五次配对结果

下表 QPS delta 为逐 repetition 的 `CombinedSE / NoCombine - 1` 后取中位数；
p99 为相同方向，负值表示延迟下降。

| 数据集 | nprobe | Combined QPS | NoCombine QPS | QPS median | QPS mean±std | 正向 rep | p99 median | total-byte delta |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| ESCI | 192 | 187.17 | 185.45 | +0.94% | +1.03%±0.39% | 5/5 | -1.62% | +5.33% |
| COCO | 128 | 330.17 | 310.41 | +6.26% | +6.24%±0.16% | 5/5 | -1.77% | -6.96% |
| MSMARCO | 96 | 205.57 | 188.45 | +9.06% | +9.04%±0.10% | 5/5 | -6.46% | +17.86% |
| VoxCeleb2 | 96 | 199.72 | 182.36 | +9.20% | +9.19%±0.35% | 5/5 | -7.04% | -0.37% |

表中两列绝对 QPS 也是五次运行的中位数。四个数据集的 Recall、probed、reranked
与 unique-fetch coverage 在每个配对内完全一致，所有 planner fallback 均为 0。

## 机制指标与结论边界

| 数据集 | Combined payload req/query | NoCombine payload req/query | payload reuse/query | SE credit bytes/query |
|---|---:|---:|---:|---:|
| ESCI | 69.57 | 100.00 | 30.43 | 79,435.78 |
| COCO | 86.04 | 100.00 | 13.96 | 60,645.38 |
| MSMARCO | 55.43 | 100.00 | 44.57 | 36,775.94 |
| VoxCeleb2 | 199.38 | 100.00 | 0.00 | 0.00 |

前三个数据集显示了论文希望验证的路径：合并 record 使 span 读取能够保留并复用
payload，从而减少最终 payload 请求。Vox 没有 SafeIn credit 或 payload reuse，
但 QPS 仍显著提高；它只能说明合并布局的整体系统效果，还不能作为 SafeIn-aware
cofetch 机制的直接证据。

CombinedSE 的 VEC_ONLY 请求数并不总是更少，部分数据集甚至明显更多；因此不能
把收益解释为“总请求数必然减少”。更准确的表述是：合并布局改变了向量阶段与
payload 阶段之间的 I/O 工作分配，并在前三个数据集消除了部分后续 payload I/O；
最终是否更快由请求粒度、字节量、局部性、payload reuse 和等待路径共同决定。

## 论文使用建议

1. 主消融表可报告四个数据集的 QPS、p99 与 Recall，并将 NoCombine 标为完整组件
   消融，而不是 planner-only 消融。
2. 机制表只使用 ESCI、COCO、MSMARCO 支撑 payload-aware cofetch；Vox 作为
   credit 为零的边界案例单独解释。
3. 若需要证明 SE 算法本身优于 GE，应另做同一 combined store、相同 rho 之外仅
   切换 planner 的 GE/SE 对照；本结果不替代该对照。

原始结果：

- `/home/zcq/VDB/test/recordgate_span_se_nocombine_ablation_20260718`
- `/home/zcq/VDB/test/recordgate_span_se_nocombine_smoke_20260718`
