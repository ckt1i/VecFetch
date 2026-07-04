# Round 4: Submit CPU Micro-Optimization

时间：2026-06-25T16:18:00+08:00

## 目的

Round 3 显示地址排序不是主要收益来源。本轮做低风险 submit CPU 优化：

1. `MaterializeBudgetedReadPlans()` 去掉重复排序。
   - 旧逻辑：`std::sort_heap()` 后又 `std::sort()`。
   - 新逻辑：只保留 `std::sort_heap()`。
2. 对预算路径预留：
   - `budgeted_read_plan_heap_`
   - `pending_vec_only_plans_`
   - `pending_slots_`

## 实验设置

- 数据集：`amazon_esci`, `msmarco_passage`
- `total_bits=4/ex_bits=3`
- code-slab HugePage
- `topk=100`
- `nprobe=256`
- `two-level-coarse-routing=1`
- `two-level-coarse-budget-factor=16`
- `queries=1000`
- `reps=3`

实验目录：

`/home/zcq/VDB/test/pipeline_optimization_20260625/round4_submit_cpu`

结果文件：

- raw CSV：`/home/zcq/VDB/test/pipeline_optimization_20260625/round4_submit_cpu/results/round4_submit_cpu_raw.csv`
- summary CSV：`/home/zcq/VDB/test/pipeline_optimization_20260625/round4_submit_cpu/results/round4_submit_cpu_summary.csv`
- compare CSV：`/home/zcq/VDB/test/pipeline_optimization_20260625/round4_submit_cpu/results/round4_submit_cpu_compare.csv`

## 结果

| dataset | baseline ms | submit-cpu ms | speedup | submit delta | final drain delta | recall delta |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| amazon_esci | 1.7881 | 1.7864 | 1.0009 | +0.0006 | -0.0014 | 0 |
| msmarco_passage | 3.1081 | 3.1494 | 0.9869 | +0.0007 | -0.0014 | 0 |

## 结论

本轮基本持平，不作为显著优化结论。

保留原因：

- 去掉重复排序是正确的低风险清理。
- reserve 不改变行为。
- ESCI 有极小收益，MSMARCO 倒退更可能是运行噪声或 cache 状态变化，但不支持“系统性加速”声明。

