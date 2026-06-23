# Stage2 vector_bitplanes 首轮实现与 COCO100k 结果

时间：2026-06-07

## 已实现内容

- 新增 `RaBitQExDataLayout::kVectorBitplanes`，CLI 参数为 `--rabitq-exdata-layout vector_bitplanes`。
- v15 variable ExData region 支持 `vector_bitplanes`：
  - block header: `uint32_t valid_count`
  - payload: `[lane][dim_block][bitplanes]`
  - factors: `factor_add[valid_count] + factor_rescale[valid_count]`
- `ExRaBitQPackOfficialDirectBitplanes()` / `UnpackOfficialDirectBitplanes()` 扩展到 `ex_bits=4`。
- 新增 `IPOfficialRaBitQBatchCompactVectorBitplanesMasked()`：
  - 只遍历 `lane_mask` 中的 survivor lane。
  - 每个 survivor lane 连续读取完整 vector ExData，减少 `[dim_block][lane]` 跳读。
- `ClusterProber` 已能在 official direct path 下 dispatch 到 vector kernel。
- `LoadCodes` / resident preload / parsed view 已支持新布局。

## 验证

通过的测试：

```text
./build/test_types
./build/test_ip_exrabitq
./build/test_cluster_store
./build/test_cluster_prober
git diff --check
```

覆盖点：

- layout parser / format key。
- direct bitplanes 1/2/3/4-bit pack-unpack。
- vector bitplanes masked kernel 与 scalar dot 一致。
- v15 cluster.clu 写入、解析、LoadCodes、resident preload。
- ClusterProber official Stage2 分数与手工公式一致。

## COCO100k 首轮结果

共同设置：

- `nlist=2048`
- `nprobe=64`
- `topk=10`
- `query_count=1000`
- `dynamic_safeout=1`
- `dynamic_safein=static`
- `non_safeout_candidate_budget=400`
- `fixed_vec_buffer_count=512`
- `epsilon_percentile=0.90`
- centroid/assignment 复用：
  - `/home/zcq/VDB/data/coco_100k/coco_centroid_2048.fvecs`
  - `/home/zcq/VDB/data/coco_100k/coco_cluster_id_2048.ivecs`

| layout | total_bits | ex_bits | R@10 | avg ms | QPS | p95 ms | p99 ms | cluster.clu bytes | resident code bytes | resident cluster mem bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `split3_trimmed_bitplanes` | 4 | 3 | 0.9504 | 0.3388 | 2951.4 | 0.4341 | 0.5236 | 38,658,048 | 29,282,040 | 30,882,040 |
| `vector_bitplanes` | 4 | 3 | 0.9504 | 0.3378 | 2960.7 | 0.4287 | 0.5167 | 38,658,048 | 29,282,040 | 30,882,040 |
| `vector_bitplanes` | 5 | 4 | 0.9486 | 0.3271 | 3056.9 | 0.4103 | 0.4707 | 45,244,416 | 35,682,040 | 37,282,040 |

## 结果文件

- build log:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/build_vector_bitplanes_ex3.log`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/build_vector_bitplanes_ex4.log`
- online repeat:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/vector_bitplanes_ex3_repeat/results.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/vector_bitplanes_ex4_repeat/results.json`
- fine-grained timing probe:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/vector_bitplanes_ex4/results.json`
- ex_bits=4 distribution:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/vector_bitplanes_ex4_distribution.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/vector_bitplanes_ex4_distribution.csv`

## 初步结论

1. 相同 `total_bits=4/ex_bits=3` 下，`vector_bitplanes` 与 `split3_trimmed_bitplanes` 的 recall、文件大小、resident code bytes 完全一致；avg latency 小幅下降，p95/p99 也略低。
2. `vector_bitplanes` 的主要收益来自访问局部性，而不是空间压缩；它没有比 trimmed 更小，因为两者都只写 `valid_count` lane。
3. `ex_bits=4` 的 4-bit direct path 已可运行，查询延迟较低，但文件大小按预期增加到 45.24 MB。是否能无损压到接近 `total_bits=4/ex_bits=3` 需要继续做第 4 bitplane 的分布统计；若第 4 bitplane 近似均匀，则信息论上无法稳定压到 3 bit/dim 量级。
4. 下一轮应继续做两个方向：
   - `vector_bitplanes_prefetch` 或 small-lane kernel 对比，验证是否还能降低 p95。
   - `ex_bits=4` 第 4 bitplane 稀疏/熵统计，再决定是否实现无损压缩。

## ex_bits=4 压缩可行性检查

脚本：

`rabitq_code_zip/analyze_vector_bitplanes.py`

输入：

`/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_official_1_plus_n_total5_ex4_vector_bitplanes_eps0.90/cluster.clu`

关键结果：

- stage2 payload bytes: 25,600,000
- per-plane zero 64-bit word rate: `[0.00029, 0.00029, 0.00029, 0.00029]`
- high-plane zero-word rate: 0.029%
- high-plane mask+raw elide estimated payload bytes: 25,705,528
- high-plane elide saving: -0.41%

结论：第 4 个 bitplane 几乎完全是混合 word，按 mask elide 不仅不能接近 `ex_bits=3`，还会变大。因此在 COCO100k 上，`ex_bits=4` 的无损压缩目标“接近 total_bits=4/ex_bits=3 大小”不成立；后续不应把 sparse/elide 作为默认 hot path，只能作为负例或换成有损降 bit 策略。

## 第二轮：vector_bitplanes_prefetch 与同版重复

时间：2026-06-07

本轮新增：

- `RaBitQExDataLayout::kVectorBitplanesPrefetch`，CLI 参数为 `--rabitq-exdata-layout vector_bitplanes_prefetch`。
- 与 `vector_bitplanes` 使用相同磁盘格式，只在 Stage2 kernel 内对下一个 survivor lane 的 vector record 做 `__builtin_prefetch`。
- `IPOfficialDirectBitplanesAvx512` 改为 `ex_bits=1..4` 的固定模板分支，避免 hot path 内的 runtime plane 分支。
- 测试补充：
  - parser / format key 覆盖 `vector_bitplanes_prefetch`。
  - SIMD vector kernel 与 prefetch kernel 均与 scalar dot 对齐。
  - storage round-trip 与 ClusterProber dispatch 覆盖 `ex_bits=1..4`。

通过验证：

```text
cmake --build build --target test_types test_ip_exrabitq test_cluster_store test_cluster_prober -j$(nproc)
./build/test_types && ./build/test_ip_exrabitq && ./build/test_cluster_store && ./build/test_cluster_prober
git diff --check
```

同版二进制 COCO100k 设置同上。

| layout | total_bits | ex_bits | R@10 | avg ms | QPS | p50 ms | p95 ms | p99 ms | cluster.clu bytes | resident code bytes | resident cluster mem bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `split3_trimmed_bitplanes` | 4 | 3 | 0.9504 | 0.347850 | 2874.8 | 0.341117 | 0.445380 | 0.538476 | 38,658,048 | 29,282,040 | 30,882,040 |
| `vector_bitplanes` | 4 | 3 | 0.9504 | 0.348227 | 2871.7 | 0.339370 | 0.446099 | 0.543255 | 38,658,048 | 29,282,040 | 30,882,040 |
| `vector_bitplanes_prefetch` | 4 | 3 | 0.9504 | 0.347924 | 2874.2 | 0.341168 | 0.444679 | 0.533888 | 38,658,048 | 29,282,040 | 30,882,040 |
| `vector_bitplanes` | 5 | 4 | 0.9486 | 0.330380 | 3026.8 | 0.324693 | 0.416571 | 0.475436 | 45,244,416 | 35,682,040 | 37,282,040 |
| `vector_bitplanes_prefetch` | 5 | 4 | 0.9486 | 0.329941 | 3030.8 | 0.324142 | 0.415100 | 0.474072 | 45,244,416 | 35,682,040 | 37,282,040 |

新增结果文件：

- build log:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/build_vector_bitplanes_prefetch_ex3.log`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/build_vector_bitplanes_prefetch_ex4.log`
- online:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/vector_bitplanes_prefetch_ex3_repeat/results.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/vector_bitplanes_ex3_specialized_repeat/results.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/split3_trimmed_bitplanes_specialized_repeat/results.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/vector_bitplanes_prefetch_ex4_repeat/results.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/vector_bitplanes_ex4_specialized_repeat/results.json`

本轮结论：

1. `vector_bitplanes_prefetch` 不改变 recall、文件大小或 resident memory。
2. `ex_bits=3` 和 `ex_bits=4` 下 prefetch 只带来约 0.1% 以内的 avg/p95 波动，属于噪声级收益，不应作为默认 hot path 的主要优化。
3. 固定 bits 分支后，`split3_trimmed_bitplanes`、`vector_bitplanes`、`vector_bitplanes_prefetch` 在 COCO100k 的 `ex_bits=3` 下基本持平；真正瓶颈可能已转向 Stage1、候选收集、SafeOut 判定或内存系统噪声。
4. 下一步如果继续追求 Stage2 加速，应优先做更明确改变计算形态的路线：`ex_bits=2` 的 2-bit direct compact IP、`ex_bits=4` 的 nibble direct compact IP，或对 survivor 做跨向量重新聚合的 micro-batch，而不是仅加预取。

## 第三轮：vector_nibble4

时间：2026-06-07

本轮新增：

- `RaBitQExDataLayout::kVectorNibble4`，CLI 参数为 `--rabitq-exdata-layout vector_nibble4`。
- 仅支持 official `total_bits=5/ex_bits=4`。
- 磁盘格式仍为 v15 variable ExData region，payload 为 `[lane][dim_block][nibble32B]`。
- 每个 64 维 block 使用 4 个 official RaBitQ 16 维 nibble group：
  - byte `j` 存第 `j` 维的低 4 bit 和第 `j+8` 维的高 4 bit。
  - 与 RaBitQ-Library 官方 4-bit compact storage 对齐。
- 新增 official nibble4 pack/unpack 和 `IPOfficialRaBitQBatchCompactVectorNibble4Masked()`。

验证：

```text
cmake --build build --target test_types test_ip_exrabitq test_cluster_store test_cluster_prober -j$(nproc)
./build/test_types && ./build/test_ip_exrabitq && ./build/test_cluster_store && ./build/test_cluster_prober
git diff --check
```

COCO100k `total_bits=5/ex_bits=4` 同版对比：

| layout | R@10 | avg ms | QPS | p50 ms | p95 ms | p99 ms | cluster.clu bytes | resident code bytes | resident cluster mem bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `vector_bitplanes` | 0.9486 | 0.330380 | 3026.8 | 0.324693 | 0.416571 | 0.475436 | 45,244,416 | 35,682,040 | 37,282,040 |
| `vector_bitplanes_prefetch` | 0.9486 | 0.329941 | 3030.8 | 0.324142 | 0.415100 | 0.474072 | 45,244,416 | 35,682,040 | 37,282,040 |
| `vector_nibble4` | 0.9486 | 0.312664 | 3198.3 | 0.309268 | 0.383165 | 0.434988 | 45,244,416 | 35,682,040 | 37,282,040 |

新增结果文件：

- `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/build_vector_nibble4_ex4.log`
- `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/vector_nibble4_ex4_repeat/results.json`

本轮结论：

1. `vector_nibble4` 满足本轮目标里的关键约束：同 `ex_bits=4` 下 recall 不降、`cluster.clu` 不增、resident code/memory 不增。
2. 相比 `vector_bitplanes`，avg latency 从 `0.330380 ms` 降到 `0.312664 ms`，约提升 `5.4%`；p95 从 `0.416571 ms` 降到 `0.383165 ms`，约提升 `8.0%`。
3. 这说明 `ex_bits=4` 下，官方 nibble compact IP 比 4 个 bitplane mask-add 更适合作为 hot path。
4. 它没有解决 `ex_bits=4` 的总大小接近 `total_bits=4/ex_bits=3` 的无损压缩目标；目前证据仍显示 COCO100k 的第 4 bitplane 不稀疏，单纯无损压缩难以接近 3-bit 大小。
5. 下一步应继续做 `ex_bits=2` direct compact IP，或在更多数据集上验证 `ex_bits=4` 的熵/压缩边界。

## 第四轮：vector_2bit

时间：2026-06-07

本轮新增：

- `RaBitQExDataLayout::kVector2Bit`，CLI 参数为 `--rabitq-exdata-layout vector_2bit`。
- 仅支持 official `total_bits=3/ex_bits=2`。
- 磁盘格式仍为 v15 variable ExData region，payload 为 `[lane][dim_block][official 2-bit compact block]`。
- 每个 64 维 block 使用 RaBitQ-Library 官方 2-bit compact 格式：
  - 64 维压成 16B。
  - byte `j` 存第 `j`、`j+16`、`j+32`、`j+48` 维的 4 个 2-bit code。
- 新增 official 2-bit pack/unpack 和 `IPOfficialRaBitQBatchCompactVector2BitMasked()`。

验证：

```text
cmake --build build --target test_types test_ip_exrabitq test_cluster_store test_cluster_prober -j$(nproc)
./build/test_types && ./build/test_ip_exrabitq && ./build/test_cluster_store && ./build/test_cluster_prober
git diff --check
```

COCO100k `total_bits=3/ex_bits=2` 低开销端到端对比：

共同设置：

- `nlist=2048`
- `nprobe=64`
- `topk=10`
- `query_count=1000`
- `dynamic_safeout=1`
- `dynamic_safein=static`
- `non_safeout_candidate_budget=400`
- `fixed_vec_buffer_count=512`
- `epsilon_percentile=0.90`

| layout | R@10 | avg ms | QPS | p50 ms | p95 ms | p99 ms | cluster.clu bytes | resident code bytes | resident cluster mem bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `vector_bitplanes` | 0.9532 | 0.371782 | 2689.7 | 0.359802 | 0.496264 | 0.604900 | 32,251,904 | 22,882,040 | 24,482,040 |
| `vector_2bit` | 0.9532 | 0.376670 | 2654.8 | 0.365586 | 0.499544 | 0.605682 | 32,251,904 | 22,882,040 | 24,482,040 |

诊断型 `fine-grained-timing=1` 结果仅用于观察 Stage2 规模，不用于低开销延迟对比：

| layout | avg ms diagnostic | avg Stage2 ms | avg reranked vectors |
| --- | ---: | ---: | ---: |
| `vector_bitplanes` | 0.663168 | 0.093247 | 137.738 |
| `vector_2bit` | 0.649310 | 0.084919 | 137.738 |

新增结果文件：

- build log:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/build_vector_bitplanes_ex2.log`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/build_vector_2bit_ex2.log`
- online low-overhead:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/vector_bitplanes_ex2_repeat/results.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/vector_2bit_ex2_repeat/results.json`
- online diagnostic:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/vector_bitplanes_ex2_timing/results.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/vector_2bit_ex2_timing/results.json`

本轮结论：

1. `vector_2bit` 的 recall、`cluster.clu`、resident code bytes 和 resident cluster memory 与 `vector_bitplanes` 完全一致。
2. 低开销端到端查询下，`vector_2bit` avg latency 比 `vector_bitplanes` 慢约 `1.3%`，p50/p95/p99 也没有改善。
3. 诊断模式下 `vector_2bit` 的 Stage2 时间略低，但该收益没有稳定转化为低开销端到端速度；`ex_bits=2` 当前不应默认切到官方 2-bit compact IP。
4. 对 `ex_bits=2`，`vector_bitplanes` 的两个 bitplane mask-add 很可能已经比 2-bit unpack+int-to-float FMA 更轻；下一步若继续优化，应考虑保留 bitplane 表达但做更好的 survivor micro-batch 或 small-lane，而不是替换成 official 2-bit compact。

## 第五轮：small_lane4_bitplanes

时间：2026-06-07

本轮新增：

- `RaBitQExDataLayout::kSmallLane4Bitplanes`，CLI 参数为 `--rabitq-exdata-layout small_lane4_bitplanes`。
- 仍使用 v15 variable ExData region，外层 block 保持 8-vector 语义，避免改动 Stage1/Stage2 block id 映射。
- Stage2 payload 从原先的 `[dim_block][valid_lanes][bitplanes]` 改为两个 4-lane subgroup：
  `[subgroup4][dim_block][local_lane][bitplanes]`。
- 支持 official `ex_bits=1..4`，不改量化语义。

验证：

```text
cmake --build build --target test_types test_ip_exrabitq test_cluster_store test_cluster_prober -j$(nproc)
./build/test_types && ./build/test_ip_exrabitq && ./build/test_cluster_store && ./build/test_cluster_prober
git diff --check
```

COCO100k `total_bits=4/ex_bits=3` 低开销端到端对比：

| layout | R@10 | avg ms | QPS | p50 ms | p95 ms | p99 ms | avg reranked vectors | cluster.clu bytes | resident code bytes | resident cluster mem bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `split3_trimmed_bitplanes` | 0.9504 | 0.347850 | 2874.8 | 0.341117 | 0.445380 | 0.538476 | - | 38,658,048 | 29,282,040 | 30,882,040 |
| `vector_bitplanes` | 0.9504 | 0.348227 | 2871.7 | 0.339370 | 0.446099 | 0.543255 | - | 38,658,048 | 29,282,040 | 30,882,040 |
| `small_lane4_bitplanes` | 0.9504 | 0.338932 | 2950.4 | 0.331626 | 0.431809 | 0.525433 | 99.845 | 38,658,048 | 29,282,040 | 30,882,040 |

COCO100k `total_bits=3/ex_bits=2` 低开销端到端对比：

| layout | R@10 | avg ms | QPS | p50 ms | p95 ms | p99 ms | avg reranked vectors | cluster.clu bytes | resident code bytes | resident cluster mem bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `vector_bitplanes` | 0.9532 | 0.371782 | 2689.7 | 0.359802 | 0.496264 | 0.604900 | 137.738 | 32,251,904 | 22,882,040 | 24,482,040 |
| `vector_2bit` | 0.9532 | 0.376670 | 2654.8 | 0.365586 | 0.499544 | 0.605682 | 137.738 | 32,251,904 | 22,882,040 | 24,482,040 |
| `small_lane4_bitplanes` | 0.9532 | 0.385253 | 2595.7 | 0.372642 | 0.524674 | 0.632363 | 137.738 | 32,251,904 | 22,882,040 | 24,482,040 |

新增结果文件：

- build log:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/build_small_lane4_ex3.log`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/build_small_lane4_ex2.log`
- online low-overhead:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/small_lane4_ex3_repeat/results.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/small_lane4_ex2_repeat/results.json`

本轮结论：

1. `small_lane4_bitplanes` 在 `ex_bits=3` 下是有效的 B1 路线：recall、文件大小和 resident memory 与 `split3_trimmed_bitplanes` / `vector_bitplanes` 相同，avg latency 相比 `vector_bitplanes` 降低约 `2.7%`，p95/p99 也同步改善。
2. `small_lane4_bitplanes` 在 `ex_bits=2` 下是负结果：recall 和存储/内存不变，但 avg latency 比 `vector_bitplanes` 慢约 `3.6%`。
3. 这说明降低 lane packing 数量并非总是有效；收益取决于 Stage2 每个候选的 per-lane compute 成本和 survivor 分布。当前证据支持把 small-lane4 作为 `ex_bits=3` 的候选 hot path，不支持用于 `ex_bits=2`。
4. 下一步若继续路线 B，应实现 `small_lane2_bitplanes` 或 survivor micro-batch，仅在 `ex_bits=3` 上优先验证；`ex_bits=2` 暂时保留 `vector_bitplanes`。

## 第六轮：small_lane2_bitplanes

时间：2026-06-07

本轮新增：

- `RaBitQExDataLayout::kSmallLane2Bitplanes`，CLI 参数为 `--rabitq-exdata-layout small_lane2_bitplanes`。
- 与 `small_lane4_bitplanes` 一样保持外层 8-vector block 不变，只把 Stage2 payload 拆成 2-lane subgroup：
  `[subgroup2][dim_block][local_lane][bitplanes]`。
- 支持 official `ex_bits=1..4`，不改量化语义。

验证：

```text
cmake --build build --target test_types test_ip_exrabitq test_cluster_store test_cluster_prober -j$(nproc)
./build/test_types && ./build/test_ip_exrabitq && ./build/test_cluster_store && ./build/test_cluster_prober
git diff --check
```

COCO100k `total_bits=4/ex_bits=3` 低开销端到端对比：

| layout | R@10 | avg ms | QPS | p50 ms | p95 ms | p99 ms | avg reranked vectors | cluster.clu bytes | resident code bytes | resident cluster mem bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `split3_trimmed_bitplanes` | 0.9504 | 0.347850 | 2874.8 | 0.341117 | 0.445380 | 0.538476 | 99.845 | 38,658,048 | 29,282,040 | 30,882,040 |
| `vector_bitplanes` | 0.9504 | 0.348227 | 2871.7 | 0.339370 | 0.446099 | 0.543255 | 99.845 | 38,658,048 | 29,282,040 | 30,882,040 |
| `small_lane4_bitplanes` | 0.9504 | 0.338932 | 2950.4 | 0.331626 | 0.431809 | 0.525433 | 99.845 | 38,658,048 | 29,282,040 | 30,882,040 |
| `small_lane2_bitplanes` | 0.9504 | 0.342380 | 2920.7 | 0.336761 | 0.439149 | 0.529523 | 99.845 | 38,658,048 | 29,282,040 | 30,882,040 |

新增结果文件：

- build log:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/build_small_lane2_ex3.log`
- online low-overhead:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/small_lane2_ex3_repeat/results.json`

本轮结论：

1. `small_lane2_bitplanes` 在 `ex_bits=3` 下也是正结果：recall、文件大小和 resident memory 不变，avg latency 相比 `vector_bitplanes` 降低约 `1.7%`。
2. 但 `small_lane2_bitplanes` 慢于 `small_lane4_bitplanes` 约 `1.0%`，p50/p95/p99 也都不如 4-lane。
3. 这说明从 8-lane 降到 4-lane 捕获了主要局部性收益；继续降到 2-lane 后，subgroup 遍历和更碎的内层循环开销开始抵消收益。
4. 当前最佳 lane-count 路线是 `small_lane4_bitplanes`，仅推荐用于 `ex_bits=3`；`small_lane2_bitplanes` 保留为对照结果。

## 第七轮：ex_bits=4 压缩边界复核

时间：2026-06-07

本轮不改 hot path，只复核 `ex_bits=4` 的无损压缩可行性，避免把 `vector_bitplanes` 的解析结果误判为布局特有现象。

复核对象：

- `vector_bitplanes`：
  `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_official_1_plus_n_total5_ex4_vector_bitplanes_eps0.90/cluster.clu`
- `vector_nibble4`：
  `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_official_1_plus_n_total5_ex4_vector_nibble4_eps0.90/cluster.clu`

结果文件：

- `/home/zcq/VDB/test/rabitq_code_zip_20260606/vector_bitplanes_ex4_distribution_v2.json`
- `/home/zcq/VDB/test/rabitq_code_zip_20260606/vector_nibble4_ex4_distribution.json`
- `/home/zcq/VDB/test/rabitq_code_zip_20260606/vector_bitplanes_ex4_distribution_v2.csv`
- `/home/zcq/VDB/test/rabitq_code_zip_20260606/vector_nibble4_ex4_distribution.csv`

两种布局解码出的统计完全一致：

| layout | records | payload bytes | per-plane zero-word rate | high-plane elide bytes | estimated saving |
| --- | ---: | ---: | --- | ---: | ---: |
| `vector_bitplanes` | 100,000 | 25,600,000 | `[0.00029, 0.00029, 0.00029, 0.00029]` | 25,705,528 | -0.41% |
| `vector_nibble4` | 100,000 | 25,600,000 | `[0.00029, 0.00029, 0.00029, 0.00029]` | 25,705,528 | -0.41% |

`value_hist` 也一致，码值 0..15 均有大量出现，且两端值并不稀疏：

```text
0: 3,889,174
1: 3,719,455
2: 3,547,637
3: 3,319,634
4: 3,067,594
5: 2,840,180
6: 2,660,647
7: 2,563,872
8: 2,565,450
9: 2,661,280
10: 2,837,968
11: 3,069,086
12: 3,318,456
13: 3,547,876
14: 3,714,301
15: 3,877,390
```

本轮结论：

1. `ex_bits=4` 的高 bitplane 非稀疏不是 `vector_bitplanes` 解析 artifact；`vector_nibble4` 复核得到相同分布。
2. 对 COCO100k，mask + raw elide 这类无损稀疏压缩不仅不能接近 `total_bits=4/ex_bits=3`，还会让 Stage2 payload 变大约 `0.41%`。
3. 因此当前可成立的结论是：`vector_nibble4` 是 `ex_bits=4` 的计算优化；`ex_bits=4` 的无损空间优化在 COCO100k 上没有可用稀疏性证据。
4. 后续若要做跨数据集泛化判断，需要先构建对应数据集的 official `total_bits=5/ex_bits=4` 索引；现有 `amazon_esci`、`voxceleb2`、`imagenet1k` 目录下主要是旧方法或 legacy bits4 索引，不能直接证明 official ex4 分布。

## 第八轮：vector_bitplanes_microbatch

时间：2026-06-07

本轮新增 `vector_bitplanes_microbatch`：

- 存储格式与 `vector_bitplanes` 完全一致：逐向量 bitplane compact record。
- 查询时根据 Stage1/Stage2 survivor lane 收集实际需要计算的 lane，再按最多 4 个 survivor lane 组成 micro-batch。
- micro-batch 内按 dim_block 遍历，尝试复用 query load；SIMD 仍沿维度方向并行。

验证：

```text
git diff --check
python3 -m py_compile rabitq_code_zip/analyze_vector_bitplanes.py
cmake --build build --target test_types test_ip_exrabitq test_cluster_store test_cluster_prober -j$(nproc)
./build/test_types && ./build/test_ip_exrabitq && ./build/test_cluster_store && ./build/test_cluster_prober
cmake --build build --target bench_build_index bench_e2e -j$(nproc)
```

COCO100k `total_bits=4/ex_bits=3` 低开销端到端对比：

| layout | R@10 | avg ms | QPS | p50 ms | p95 ms | p99 ms | avg reranked vectors | Stage2 ms | cluster.clu bytes | resident code bytes | resident cluster mem bytes | peak query RSS KiB |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `vector_bitplanes` | 0.9504 | 0.337759 | 2960.7 | 0.332188 | 0.428651 | 0.516676 | 99.845 | 0.067374 | 38,658,048 | 29,282,040 | 30,882,040 | 86,756 |
| `small_lane4_bitplanes` | 0.9504 | 0.338932 | 2950.4 | 0.331626 | 0.431809 | 0.525433 | 99.845 | 0.070318 | 38,658,048 | 29,282,040 | 30,882,040 | 86,708 |
| `vector_bitplanes_microbatch` | 0.9504 | 0.342787 | 2917.3 | 0.334955 | 0.444292 | 0.534281 | 99.845 | 0.073778 | 38,658,048 | 29,282,040 | 30,882,040 | 86,676 |

新增结果文件：

- build log:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/build_vector_bitplanes_microbatch_ex3.log`
- online low-overhead:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/vector_bitplanes_microbatch_ex3_repeat/results.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/online_vector_bitplanes_microbatch_ex3.log`
- index:
  - `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_official_1_plus_n_total4_ex3_vector_bitplanes_microbatch_eps0.90`

本轮结论：

1. `vector_bitplanes_microbatch` 是正确但负向的实验：recall、rerank 数、文件大小和 resident memory 均与 `vector_bitplanes` 一致。
2. avg latency 比同轮 `vector_bitplanes` 慢约 `1.5%`，Stage2 时间从 `0.067374 ms` 增至 `0.073778 ms`。
3. 说明在 COCO100k 当前 survivor 分布下，micro-batch 节省的 query load 不足以抵消 lane 收集、分组调度和多 accumulator 循环开销。
4. `vector_bitplanes_microbatch` 不进入默认 hot path。早期 `small_lane4_bitplanes` 的正收益结论已在第十一轮多次重复复核中被修正：最终默认路线应回到 `vector_bitplanes` / `vector_bitplanes_prefetch`。

## 第九轮：voxceleb2 ex_bits=4 压缩边界复核

时间：2026-06-07

目的：

- 第七轮只在 COCO100k 上证明 `ex_bits=4` 高 bitplane 不稀疏。
- 本轮补建一个非图像数据集的 official `total_bits=5/ex_bits=4` 索引，检查无损稀疏压缩失败是否是 COCO 特例。

本轮构造了一个仅包含符号链接的临时数据目录：

- `/home/zcq/VDB/test/rabitq_code_zip_20260606/datasets/voxceleb2_ecapa_150k_split_v1`
- base embedding 指向：
  `/home/zcq/VDB/data/formal_baselines/voxceleb2_ecapa_150k/embeddings/base_embeddings.npy`
- query split 指向：
  `/home/zcq/VDB/data/bench_e2e/voxceleb2_ecapa_150k_split_v1/query_embeddings.npy`
- 聚类文件：
  - `/home/zcq/VDB/data/bench_e2e/voxceleb2_ecapa_150k/clustering/voxceleb2_ecapa_150k_centroid_2048.fvecs`
  - `/home/zcq/VDB/data/bench_e2e/voxceleb2_ecapa_150k/clustering/voxceleb2_ecapa_150k_cluster_id_2048.ivecs`

新增 official ex4 索引：

- `/home/zcq/VDB/test/rabitq_code_zip_20260606/build_outputs_vox_ex4/voxceleb2_ecapa_150k_split_v1_20260607T031834/index_official_1_plus_n_total5_ex4_vector_bitplanes`

结果文件：

- build log:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/build_vox_vector_bitplanes_ex4.log`
- distribution:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/voxceleb2_vector_bitplanes_ex4_distribution.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/voxceleb2_vector_bitplanes_ex4_distribution.csv`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/analyze_vox_vector_bitplanes_ex4.log`

构建信息：

| dataset | records | dim | nlist | layout | cluster.clu bytes | data.dat bytes |
| --- | ---: | ---: | ---: | --- | ---: | ---: |
| `voxceleb2_ecapa_150k_split_v1` | 150,000 | 192 | 2048 | `vector_bitplanes` | 27,643,904 | 614,400,000 |

ex4 分布与 COCO 对比：

| dataset | records | dim blocks | payload bytes | per-plane zero-word rate | high-plane elide bytes | estimated saving |
| --- | ---: | ---: | ---: | --- | ---: | ---: |
| COCO100k | 100,000 | 8 | 25,600,000 | `[0.00029, 0.00029, 0.00029, 0.00029]` | 25,705,528 | -0.412% |
| voxceleb2 | 150,000 | 3 | 14,400,000 | `[0.0, 0.0, 0.0, 0.0]` | 14,458,947 | -0.409% |

voxceleb2 `value_hist`：

```text
0: 2,133,130
1: 2,040,499
2: 1,958,338
3: 1,851,807
4: 1,733,693
5: 1,624,197
6: 1,539,909
7: 1,497,641
8: 1,498,442
9: 1,544,621
10: 1,632,035
11: 1,743,976
12: 1,860,191
13: 1,963,991
14: 2,044,422
15: 2,133,108
```

本轮结论：

1. voxceleb2 上四个 bitplane 的全零 word 率均为 `0`，比 COCO 更不支持 sparse/elide 类无损压缩。
2. `mask + raw high-plane elide` 会让 Stage2 payload 从 `14.4 MB` 增至 `14.459 MB`，变大约 `0.409%`。
3. `ex_bits=4` 若要接近 `total_bits=4/ex_bits=3` 的空间，需要把每 64 维 32B payload 压到接近 24B；但 COCO 和 voxceleb2 的 4-bit 码值分布都没有可利用的高 bitplane 稀疏性。
4. 因此当前证据支持停止 sparse/elide 方向；`ex_bits=4` 的空间目标若仍要继续，只能转向块级通用熵压缩并承担随机访问解压成本，或者在论文中明确报告“无损空间压缩不成立，仅保留 `vector_nibble4` 的计算优化”。

## 第十轮：ex_bits=4 块级熵压缩评估

时间：2026-06-07

本轮扩展 `rabitq_code_zip/analyze_vector_bitplanes.py`：

- 新增 `value_entropy_bits_per_dim`，用码值直方图计算 4-bit code 的信息熵。
- 新增 `--compression-algorithms`：
  `zlib1/zlib6/lzma0/lz4/zstd1/zstd3/zstd6`。
- 新增 `--compression-block-sizes`，本轮使用 `4096,16384,65536`。
- 压缩对象只包含 Stage2 ex-code payload，并计入独立随机访问需要的 `(num_blocks + 1) * 8` offset table。
- 随机访问代价通过随机采样 compressed block 并实际解压得到。

结果文件：

- COCO zlib/lzma:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/coco_vector_bitplanes_ex4_block_compression.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/coco_vector_bitplanes_ex4_block_compression.csv`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/analyze_coco_vector_bitplanes_ex4_block_compression.log`
- COCO lz4/zstd:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/coco_vector_bitplanes_ex4_block_compression_zstd_lz4.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/coco_vector_bitplanes_ex4_block_compression_zstd_lz4.csv`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/analyze_coco_vector_bitplanes_ex4_block_compression_zstd_lz4.log`
- voxceleb2 zlib/lzma:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/voxceleb2_vector_bitplanes_ex4_block_compression.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/voxceleb2_vector_bitplanes_ex4_block_compression.csv`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/analyze_vox_vector_bitplanes_ex4_block_compression.log`
- voxceleb2 lz4/zstd:
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/voxceleb2_vector_bitplanes_ex4_block_compression_zstd_lz4.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/voxceleb2_vector_bitplanes_ex4_block_compression_zstd_lz4.csv`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/analyze_vox_vector_bitplanes_ex4_block_compression_zstd_lz4.log`

信息熵下界：

| dataset | ex4 payload bytes | ex3 target bytes | value entropy bit/dim | ideal entropy / ex3 target |
| --- | ---: | ---: | ---: | ---: |
| COCO100k | 25,600,000 | 19,200,000 | 3.984887 | 1.3283x |
| voxceleb2 | 14,400,000 | 10,800,000 | 3.989137 | 1.3297x |

实际块压缩最佳结果：

| dataset | best codec | block size | compressed bytes incl. offset table | saving vs ex4 payload | ratio to ex3 target | random block decompress |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| COCO100k | `zlib6` | 65,536 | 25,604,614 | -0.018% | 1.3336x | 26.33 us |
| COCO100k | `zstd1` | 65,536 | 25,607,046 | -0.028% | 1.3337x | 3.20 us |
| voxceleb2 | `zstd1` | 65,536 | 14,403,968 | -0.028% | 1.3337x | 2.52 us |
| voxceleb2 | `zlib6` | 65,536 | 14,406,725 | -0.047% | 1.3340x | 25.69 us |

随机访问粒度影响：

| dataset | codec | block size | compressed bytes | ratio to ex3 target | random block decompress |
| --- | --- | ---: | ---: | ---: | ---: |
| COCO100k | `zstd6` | 4,096 | 25,704,555 | 1.3388x | 0.80 us |
| COCO100k | `zstd6` | 16,384 | 25,626,675 | 1.3347x | 1.34 us |
| COCO100k | `zstd6` | 65,536 | 25,607,046 | 1.3337x | 3.18 us |
| voxceleb2 | `zstd6` | 4,096 | 14,463,043 | 1.3392x | 0.75 us |
| voxceleb2 | `zstd6` | 16,384 | 14,415,830 | 1.3348x | 1.23 us |
| voxceleb2 | `zstd6` | 65,536 | 14,403,968 | 1.3337x | 3.01 us |

本轮结论：

1. 信息熵已经给出强负结果：COCO 和 voxceleb2 的 ex4 code 熵接近 `4 bit/dim`，而接近 ex3 空间需要接近 `3 bit/dim`。
2. 实际 zlib/lzma/lz4/zstd 独立块压缩没有带来空间收益；最佳结果仍略大于原始 ex4 payload。
3. 4KB 小块虽然随机解压只需约 `0.75-0.80 us/block`，但 offset 和块头开销更高，距离 ex3 目标更远。
4. 64KB 大块压缩率最好，但随机访问要解压更大的无关 payload；即便解压开销可控，空间仍是 ex3 目标的约 `1.334x`。
5. 因此 `ex_bits=4` 的“接近 total_bits=4/ex_bits=3 大小”的无损压缩目标在当前 RaBitQ code 分布下不成立。论文中应把这一点写成负结果或边界分析；生产默认路径不应引入块级熵压缩。

## 第十一轮：ex_bits=3 最终路线选择复核

时间：2026-06-07

目的：

- 早期单次结果显示 `small_lane4_bitplanes` 在 `ex_bits=3` 下可能比 `vector_bitplanes` 快。
- 后续 microbatch 轮次中 `vector_bitplanes` 又略快于 `small_lane4_bitplanes`。
- 本轮复用已有索引，只跑在线查询，做多次重复和交错顺序复核，避免把单次噪声或运行顺序偏差当成路线结论。

候选索引：

- `split3_trimmed`：
  `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_official_1_plus_n_total4_ex3_split3_trimmed_bitplanes_eps0.90`
- `vector_bitplanes`：
  `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_official_1_plus_n_total4_ex3_vector_bitplanes_eps0.90`
- `vector_bitplanes_prefetch`：
  `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_official_1_plus_n_total4_ex3_vector_bitplanes_prefetch_eps0.90`
- `small_lane4`：
  `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_official_1_plus_n_total4_ex3_small_lane4_bitplanes_eps0.90`
- `small_lane2`：
  `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_official_1_plus_n_total4_ex3_small_lane2_bitplanes_eps0.90`
- `microbatch`：
  `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_official_1_plus_n_total4_ex3_vector_bitplanes_microbatch_eps0.90`

实验设置：

- COCO100k
- `nlist=2048`
- `nprobe=64`
- `query_count=1000`
- `topk=10`
- `non_safeout_candidate_budget=400`
- `dynamic_safeout=1`
- `dynamic_safein=static`
- `fixed_vec_buffer_count=512`
- `rabitq_validation_mode=official_1_plus_n`

结果文件：

- 顺序重复 5 次：
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/ex3_route_selection_20260607T0328/`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/ex3_route_selection_20260607T0328/`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/ex3_route_selection_20260607T0328_summary.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/ex3_route_selection_20260607T0328_summary.csv`
- 交错顺序重复 5 次：
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/online/ex3_route_selection_interleaved_20260607T0330/`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/ex3_route_selection_interleaved_20260607T0330/`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/ex3_route_selection_interleaved_20260607T0330_summary.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/ex3_route_selection_interleaved_20260607T0330_summary.csv`
- 合并关键候选 10 次：
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/ex3_route_selection_combined_20260607T0330_summary.json`
  - `/home/zcq/VDB/test/rabitq_code_zip_20260606/ex3_route_selection_combined_20260607T0330_summary.csv`

第一轮顺序重复 5 次：

| layout | R@10 | avg ms mean | QPS mean | p95 ms mean | p99 ms mean | Stage2 ms mean | avg reranked | resident code bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `vector_bitplanes` | 0.9504 | 0.338566 | 2954.3 | 0.433664 | 0.515436 | 0.069022 | 99.845 | 29,282,040 |
| `vector_bitplanes_prefetch` | 0.9504 | 0.339340 | 2947.1 | 0.436188 | 0.521383 | 0.068627 | 99.845 | 29,282,040 |
| `split3_trimmed` | 0.9504 | 0.340535 | 2936.7 | 0.437037 | 0.527002 | 0.071063 | 99.845 | 29,282,040 |
| `small_lane4` | 0.9504 | 0.344411 | 2903.9 | 0.441703 | 0.529227 | 0.072548 | 99.845 | 29,282,040 |
| `small_lane2` | 0.9504 | 0.344864 | 2899.9 | 0.443035 | 0.527744 | 0.073470 | 99.845 | 29,282,040 |
| `microbatch` | 0.9504 | 0.347156 | 2881.1 | 0.449401 | 0.529137 | 0.075711 | 99.845 | 29,282,040 |

第二轮交错顺序重复 5 次：

| layout | R@10 | avg ms mean | QPS mean | p95 ms mean | p99 ms mean | Stage2 ms mean | avg reranked | resident code bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `vector_bitplanes_prefetch` | 0.9504 | 0.335025 | 2984.9 | 0.427782 | 0.504466 | 0.067585 | 99.845 | 29,282,040 |
| `vector_bitplanes` | 0.9504 | 0.336295 | 2973.7 | 0.429423 | 0.507778 | 0.068324 | 99.845 | 29,282,040 |
| `small_lane4` | 0.9504 | 0.343032 | 2915.4 | 0.438274 | 0.526229 | 0.072365 | 99.845 | 29,282,040 |
| `split3_trimmed` | 0.9504 | 0.344501 | 2903.2 | 0.443336 | 0.528930 | 0.072028 | 99.845 | 29,282,040 |

合并关键候选 10 次：

| layout | R@10 | avg ms mean | avg ms stdev | QPS mean | p95 ms mean | p99 ms mean | Stage2 ms mean | resident code bytes |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `vector_bitplanes_prefetch` | 0.9504 | 0.337183 | 0.003105 | 2966.0 | 0.431985 | 0.512925 | 0.068106 | 29,282,040 |
| `vector_bitplanes` | 0.9504 | 0.337431 | 0.004171 | 2964.0 | 0.431544 | 0.511607 | 0.068673 | 29,282,040 |
| `split3_trimmed` | 0.9504 | 0.342518 | 0.004173 | 2919.9 | 0.440187 | 0.527966 | 0.071546 | 29,282,040 |
| `small_lane4` | 0.9504 | 0.343722 | 0.003778 | 2909.6 | 0.439989 | 0.527728 | 0.072457 | 29,282,040 |

本轮结论：

1. `vector_bitplanes` / `vector_bitplanes_prefetch` 是最终 `ex_bits=3` 路线；两者 recall、存储和 resident memory 完全相同。
2. `vector_bitplanes_prefetch` 的合并 avg latency 比 `vector_bitplanes` 低约 `0.07%`，但差距远小于运行噪声；`vector_bitplanes` 的 p95/p99 反而略好。因此默认应选更简单的 `vector_bitplanes`，`prefetch` 作为可选变体保留。
3. `small_lane4` 的早期正收益没有复现；合并 10 次后它比 `vector_bitplanes` 慢约 `1.86%`，Stage2 也更慢。
4. `small_lane2` 和 `microbatch` 均为负结果，不进入默认 hot path。
5. 与旧 `split3_trimmed` 相比，默认 `vector_bitplanes` 保持 R@10、文件大小、resident memory 和 rerank 数不变，avg latency 提升约 `1.49%`，Stage2 时间下降约 `4.0%`。

## 第十二轮：默认 layout 策略落地验证

时间：2026-06-07

目的：

- 第十一轮只给出了最终路线选择；本轮把选择落实到代码默认行为。
- 兼容性原则：不改变磁盘上 `generic_packed` 的含义，避免旧索引读取被误解析。
- 构建默认：当 official 1+n 构建没有显式传 `--rabitq-exdata-layout` 时，按最终路线自动选择。

代码变更：

- `RaBitQResolveSelectedExDataLayoutForBits(selected_direct, ex_bits)`：
  - `ex_bits=1,2,3` -> `vector_bitplanes`
  - `ex_bits=4` -> `vector_nibble4`
- 新增 `RaBitQDefaultOfficialExDataLayoutForBits(ex_bits)`：
  - `ex_bits=0` -> `generic_packed`
  - `ex_bits=1,2,3` -> `vector_bitplanes`
  - `ex_bits=4` -> `vector_nibble4`
- `bench_e2e.cpp` / `bench_build_index`：
  - 如果用户没有显式传 `--rabitq-exdata-layout`，official 1+n 构建使用上述默认 layout。
  - 如果用户显式传 `generic_packed`，仍保留旧 generic 行为。

smoke 数据：

- `/home/zcq/VDB/test/rabitq_code_zip_20260606/default_layout_smoke_dataset`
- 输出目录：
  `/home/zcq/VDB/test/rabitq_code_zip_20260606/default_layout_smoke_outputs`
- 日志：
  `/home/zcq/VDB/test/rabitq_code_zip_20260606/logs/default_layout_smoke/`

smoke 验证：

| command setting | explicit layout? | generated index dir |
| --- | --- | --- |
| official `total_bits=4/ex_bits=3` | no | `index_official_1_plus_n_total4_ex3_vector_bitplanes` |
| official `total_bits=5/ex_bits=4` | no | `index_official_1_plus_n_total5_ex4_vector_nibble4` |

验证命令：

```text
cmake --build build --target test_types bench_build_index -j$(nproc)
./build/test_types
cmake --build build --target test_ip_exrabitq test_cluster_store test_cluster_prober -j$(nproc)
./build/test_ip_exrabitq && ./build/test_cluster_store && ./build/test_cluster_prober
```

本轮结论：

1. 最终路线已从实验结论落到默认构建行为。
2. 旧索引兼容性保留：读取已有 `generic_packed` 不会被自动解释成新 layout。
3. `selected_direct` 现在也映射到最终 direct 路线，可用于显式请求“当前推荐 direct layout”。
