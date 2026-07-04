# Round5：tile_lane_bitmajor 的 8-lane hot path 收缩

日期：2026-06-28

## 背景

当前 `tile_lane_bitmajor` 的 Stage2 kernel 面向 batch block 计算。实际索引路径中，Stage2 batch size 是 8 lane，但 AVX512 hot path 中的临时数组仍按 32 lane 分配：

```text
lanes[32]
dot0[32], dot1[32], dot2[32]
```

这会增加栈空间和潜在寄存器/调度压力。虽然不一定是主瓶颈，但这是一个低风险优化。

## 修改方案

在 `IPOfficialTileLaneBitMajorAvx512FixedStored` 中：

- 将最大 lane 数限制为 8；
- 将 `lanes/dot0/dot1/dot2` 临时数组从 32 缩为 8；
- 若外部误传 `valid_count > 8`，直接回退到不执行 hot path，避免越界或误算。

## 预期

- 不改变存储格式、结果或 recall。
- 可能小幅降低 Stage2 kernel 开销。
- 若无收益，则说明当前差距已经主要不在该 kernel 的临时数组开销。

## 验证

1. `test_ip_exrabitq`
2. `test_cluster_prober`
3. Amazon/MSMARCO `topk=10,nprobe=64,queries=1000,REPS=5` fixed active=3 对比。

## 结果

相关测试通过：

- `test_ip_exrabitq`
- `test_cluster_prober`

`topk=10,nprobe=64,queries=1000,REPS=5`：

| dataset | layout | avg ms mean | Stage2 ms mean | recall@10 |
| --- | --- | ---: | ---: | ---: |
| Amazon ESCI | vector_bitplanes | 0.5149 | 0.0539 | 0.7668 |
| Amazon ESCI | tile_lane_bitmajor + lane8 trim | 0.5136 | 0.0518 | 0.7668 |
| MSMARCO Passage | vector_bitplanes | 1.2635 | 0.1597 | 0.8101 |
| MSMARCO Passage | tile_lane_bitmajor + lane8 trim | 1.2632 | 0.1549 | 0.8101 |

相对上一版 Round2/4 的 REPS=5：

| dataset | 旧 tile avg ms | Round5 tile avg ms | 旧 Stage2 ms | Round5 Stage2 ms |
| --- | ---: | ---: | ---: | ---: |
| Amazon ESCI | 0.5065 | 0.5136 | 0.0501 | 0.0518 |
| MSMARCO Passage | 1.2540 | 1.2632 | 0.1525 | 0.1549 |

## 结论

该优化为负收益，已从代码中撤回。可能原因是：

- 32-lane 数组虽然看起来过大，但编译器对实际 `lane_count <= 8` 的循环已经处理较好；
- 改动引入的额外 `valid_count > 8` 分支和不同栈布局没有改善 hot path；
- 当前端到端缺口主要不在这个临时数组规模上。

最终保留 Round2 fused bit-plane kernel，不采用 Round5。
