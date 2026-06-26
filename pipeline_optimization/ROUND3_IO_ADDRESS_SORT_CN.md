# Round 3: I/O Address-Local Submit Sorting

时间：2026-06-25T16:10:00+08:00

## 目的

参考 LAANN 的 I/O 调度思想，本轮尝试在同一批 vec-only read 内提升地址局部性：

- 候选选择不变。
- 只改变同一 emit 窗口内的 `data.dat` offset 顺序。
- 新增开关：
  - `--vec-read-address-sort 1`
  - `--vec-read-address-sort-window N`

窗口设置：

- `64`
- `128`
- `0`：整个 emit window 排序。

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

`/home/zcq/VDB/test/pipeline_optimization_20260625/round3_io_address_sort`

结果文件：

- raw CSV：`/home/zcq/VDB/test/pipeline_optimization_20260625/round3_io_address_sort/results/round3_io_address_sort_raw.csv`
- summary CSV：`/home/zcq/VDB/test/pipeline_optimization_20260625/round3_io_address_sort/results/round3_io_address_sort_summary.csv`
- compare CSV：`/home/zcq/VDB/test/pipeline_optimization_20260625/round3_io_address_sort/results/round3_io_address_sort_compare.csv`

## 结果

| dataset | window | baseline ms | sorted ms | speedup | submit delta | final drain delta | sort overhead | recall delta |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| amazon_esci | 64 | 1.7881 | 1.8049 | 0.9907 | +0.0038 | -0.0010 | 0.0027 | 0 |
| amazon_esci | 128 | 1.7881 | 1.7999 | 0.9934 | +0.0056 | -0.0008 | 0.0030 | 0 |
| amazon_esci | 0 | 1.7881 | 1.7945 | 0.9964 | +0.0058 | -0.0025 | 0.0034 | 0 |
| msmarco_passage | 64 | 3.1081 | 3.1421 | 0.9892 | +0.0278 | -0.0015 | 0.0051 | 0 |
| msmarco_passage | 128 | 3.1081 | 3.1540 | 0.9855 | +0.0281 | -0.0018 | 0.0058 | 0 |
| msmarco_passage | 0 | 3.1081 | 3.1207 | 0.9960 | +0.0202 | -0.0030 | 0.0067 | 0 |

## 结论

本轮是负优化，不进入默认路径。

观察：

1. 地址排序可以略微降低 `final_drain_ms`，但幅度只有 `0.001-0.003 ms`。
2. 排序本身和 emit 路径开销更高，导致总延迟变慢。
3. whole-window 排序相对最好，但仍没有超过 baseline。

保留方式：

- 代码保留为可选开关，默认关闭。
- 后续若在更慢 SSD 或更大 payload 上测试，可以重新打开验证。

