# Round 2: Stage1 Build/Preload-Time Block Envelope

时间：2026-06-25T15:42:00+08:00

## 目的

此前 runtime block envelope 的问题是每个 query 对每个 FastScan block 重复扫描：

- packed codes 的 nibble presence；
- block 内 `norm_oc` 的 min/max。

本轮尝试把这两类 query-independent 信息移到 preload 阶段预计算：

- `VDB_STAGE1_PRECOMPUTE_ENVELOPE=1`
- 查询时继续使用 `--stage1-block-skip-envelope 1`

磁盘格式不变；旧索引可复用。预计算 summary 存在 resident memory 中。

## 代码改动

- `include/vdb/query/parsed_cluster.h`
  - 增加 Stage1 envelope summary 指针和 owned storage。
- `include/vdb/storage/cluster_store.h`
  - `ResidentClusterView` 增加 summary storage。
- `src/storage/cluster_store.cpp`
  - 在 resident preload 阶段按需构建每个 block 的：
    - `uint16_t presence_mask[group]`
    - `norm_min`
    - `norm_max`
  - 新增 `resident_stage1_envelope_bytes` 统计。
- `src/index/cluster_prober.cpp`
  - 若 summary 存在，则 envelope 直接用预计算 presence/norm。
  - 若 summary 不存在，则回退此前 runtime envelope。
- `benchmarks/bench_online_query.cpp` / `benchmarks/bench_e2e.cpp`
  - 输出 `resident_stage1_envelope_bytes`。

## 实验设置

最小化 sweep：

- 数据集：`amazon_esci`, `msmarco_passage`
- `total_bits=4/ex_bits=3`
- `topk=100`
- `nprobe=256`
- `two-level-coarse-routing=1`
- `two-level-coarse-budget-factor=16`
- `queries=1000`
- `reps=3`
- 对比 Round 1 warmup 后 baseline。

实验目录：

`/home/zcq/VDB/test/pipeline_optimization_20260625/round2_stage1_precompute_envelope`

结果文件：

- raw CSV：`/home/zcq/VDB/test/pipeline_optimization_20260625/round2_stage1_precompute_envelope/results/round2_stage1_precompute_envelope_raw.csv`
- summary CSV：`/home/zcq/VDB/test/pipeline_optimization_20260625/round2_stage1_precompute_envelope/results/round2_stage1_precompute_envelope_summary.csv`
- compare CSV：`/home/zcq/VDB/test/pipeline_optimization_20260625/round2_stage1_precompute_envelope/results/round2_stage1_precompute_envelope_compare.csv`

## 结果

| dataset | mode | baseline ms | precompute-envelope ms | speedup | Stage1 baseline | Stage1 envelope | skip rate | extra memory |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| amazon_esci | compact_off | 1.8670 | 7.8394 | 0.2382 | 0.4958 | 5.8598 | 0.468% | 22.7 MiB |
| amazon_esci | code-slab HP | 1.7881 | 7.7581 | 0.2305 | 0.4447 | 5.8023 | 0.468% | 22.7 MiB |
| msmarco_passage | compact_off | 3.2396 | 18.7075 | 0.1732 | 1.2487 | 16.0854 | 0.277% | 106.2 MiB |
| msmarco_passage | code-slab HP | 3.1081 | 18.6249 | 0.1669 | 1.1513 | 16.0507 | 0.277% | 106.2 MiB |

recall@100 delta 均为 `0`。

## 结论

本轮是负优化，不进入主路径。

原因：

1. 预计算只消除了 per-query 的 presence/norm 扫描。
2. 真正昂贵的是每个 query 仍需对 `block × group` 做 LUT 上界求和。
3. skip 命中率极低：
   - ESCI 约 `0.47%`
   - MSMARCO 约 `0.28%`
4. 即使 summary 内存开销可以接受，Stage1 envelope 本身也没有足够裁剪收益。

保留方式：

- 代码保留为诊断开关。
- 默认不设置 `VDB_STAGE1_PRECOMPUTE_ENVELOPE`。
- 默认不启用 `--stage1-block-skip-envelope`。

下一步：

- 停止 Stage1 block envelope 路线。
- 按 LAANN 映射转向 submit / I/O pipeline：
  - priority submit queue；
  - 小窗口地址局部性排序；
  - pending read backlog 重判；
  - tail timing 继续拆分。
