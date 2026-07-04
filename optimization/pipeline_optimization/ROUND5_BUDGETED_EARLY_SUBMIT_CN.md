# Round 5: Budgeted Early Submit

时间：2026-06-25T16:28:00+08:00

## 目的

当前 `non_safeout_candidate_budget=400` 路径会先收集候选预算 heap，等所有 probed clusters 完成后再 materialize 和 submit。这样保证候选选择严格，但 I/O 与后续 probe 几乎没有重叠。

参考 LAANN 的“高优先级候选及时发起 I/O，等待期间继续做 CPU work”思想，本轮新增可选 early-submit：

- 当 budget heap 已满；
- 每隔固定 cluster 数；
- 复制当前 heap，按 rank 取前 N；
- 提前 submit 这些当前最优候选；
- 最终 materialize 时跳过已经提前提交的 offset。

新增开关：

- `--budgeted-early-submit 1`
- `--budgeted-early-submit-interval-clusters`
- `--budgeted-early-submit-count`
- `--budgeted-early-submit-max`

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

测试设置：

- `interval32_count32_max64`
- `interval32_count64_max128`

实验目录：

`/home/zcq/VDB/test/pipeline_optimization_20260625/round5_budgeted_early_submit`

结果文件：

- raw CSV：`/home/zcq/VDB/test/pipeline_optimization_20260625/round5_budgeted_early_submit/results/round5_budgeted_early_submit_raw.csv`
- summary CSV：`/home/zcq/VDB/test/pipeline_optimization_20260625/round5_budgeted_early_submit/results/round5_budgeted_early_submit_summary.csv`
- compare CSV：`/home/zcq/VDB/test/pipeline_optimization_20260625/round5_budgeted_early_submit/results/round5_budgeted_early_submit_compare.csv`

## 结果

| dataset | setting | baseline ms | early-submit ms | speedup | submit delta | final drain delta | early submitted | vec reads | recall delta |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| amazon_esci | count32 max64 | 1.7881 | 1.8379 | 0.9729 | -0.0414 | -0.0185 | 56.9 | 244.7 | 0 |
| amazon_esci | count64 max128 | 1.7881 | 1.8460 | 0.9686 | -0.0808 | -0.0327 | 113.8 | 244.7 | 0 |
| msmarco_passage | count32 max64 | 3.1081 | 3.2375 | 0.9600 | -0.0713 | -0.0197 | 62.3 | 273.4 | 0 |
| msmarco_passage | count64 max128 | 3.1081 | 3.1845 | 0.9760 | -0.1450 | -0.0355 | 124.7 | 273.4 | 0 |

## 结论

本轮不进入默认路径。

积极信号：

- early-submit 确实降低了 `submit_ms` 和 `final_drain_ms`。
- recall 不变。

负面结果：

- 总耗时变慢。
- 原因是提前提交引入额外 I/O、更多 completion/rerank pressure，以及 heap copy/sort 的 CPU 开销。
- 当前 `data.dat` 读取和 probe 重叠不足以抵消这些额外成本。

后续如果继续该方向，应改成更严格的策略：

- 只提前提交已经稳定留在 top budget 的候选；
- 或者给 early-submit 加更低的 max，例如 `16-32`；
- 或者在 I/O 等待明显更高的设备上重测。

