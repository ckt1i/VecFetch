# Pipeline Optimization Final Summary

时间：2026-06-25T16:35:00+08:00

## 已完成轮次

| round | 方向 | 结果 | 是否进入默认路径 |
| --- | --- | --- | --- |
| 1 | two-level hierarchy warmup 前移 | 有效，查询计时内 hierarchy build 归零 | 是 |
| 2 | Stage1 preload-time block envelope | 明显负优化 | 否 |
| 3 | vec-only read 地址局部性排序 | 负优化 | 否，保留开关 |
| 4 | submit CPU 微优化 | 基本持平 | 仅保留低风险清理 |
| 5 | budgeted early-submit | submit/drain 降低但总耗时变慢 | 否，保留开关 |

## 最终采用项

### 1. Two-level hierarchy warmup

`bench_online_query` 现在会在创建 scheduler 前调用：

- `SetTwoLevelCoarseRouting(...)`
- `PrepareTwoLevelCoarseRouting(nprobe)`

效果：

- `avg_coarse_hierarchy_build_ms = 0`
- ESCI 相对上一轮 two-level16 平均下降约 `9.4-10.8%`
- MSMARCO 相对上一轮 two-level16 平均下降约 `6.7-10.5%`
- 这主要是计时口径和在线初始化路径修正，不是新的搜索算法收益。

### 2. Submit CPU 小清理

保留：

- `MaterializeBudgetedReadPlans()` 去掉重复排序。
- budgeted path 预留 pending vector/slot 容量。

该项不支撑显著性能 claim，但代码更合理。

## 不采用项

### Stage1 block envelope

即使把 presence/norm summary 移到 preload，query-time 仍需 `block × group` 查 LUT 求上界。skip rate 只有：

- ESCI: `0.468%`
- MSMARCO: `0.277%`

因此该方向停止。

### Address sorting

排序略降 final drain，但排序和 emit 开销更高，总体变慢。

### Budgeted early-submit

该方向验证了 LAANN 思想中的“及时 I/O”确实能降低 submit/drain，但当前实现总耗时变慢，原因是额外 I/O 与 heap copy/sort 成本超过 overlap 收益。

## 当前瓶颈

基于 Round 1 warmup 后 code-slab HP：

| dataset | total ms | coarse | probe | Stage1 | Stage2 | submit | unaccounted |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| amazon_esci bits=4 | 1.7881 | 13.7% | 69.0% | 24.9% | 10.9% | 17.0% | 6.6% |
| msmarco bits=4 | 3.1081 | 10.7% | 72.8% | 37.0% | 8.8% | 16.0% | 10.0% |

后续最值得继续的方向：

1. Stage1 主循环本身的 SIMD/布局优化，而不是 block envelope。
2. coarse topN selection，仍有 `0.24 ms` ESCI / `0.32 ms` MSMARCO。
3. 更细粒度 tail timing，尤其 MSMARCO `unaccounted ~10%`。
4. 若继续 LAANN-style I/O，应先找更可靠的“稳定 top-budget”判定，避免 early-submit 额外读。

## 验证

已通过：

```bash
cmake --build build --target bench_e2e -j4
./build/test_cluster_prober
git diff --check
```

