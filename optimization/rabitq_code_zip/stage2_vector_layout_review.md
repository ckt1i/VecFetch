# RaBitQ Stage2 向量局部性优化方案记录

本文记录本轮实现前的调研、候选方案和实验判据。目标是针对 Stage1 剪枝后 Stage2 只访问少量候选的事实，减少 8-lane block 布局带来的无关访问与 cache 局部性损失。

## 调研依据

### 外部文献与实现经验

- RaBitQ 论文和 RaBitQ-Library 文档把总 bit 数拆成 `1 + ex_bits`：先用 1-bit binary code 做快速估计，再用 ex-code 做 incremental distance boosting。官方 IVF 查询阶段也是先扫描 1-bit FastScan batch，再只对 lower bound 通过的候选访问 ex-code。
- 官方 RaBitQ-Library 的 `ex_data` 不是 byte-per-dim，而是 `padded_dim * ex_bits / 8 + 2 * sizeof(float)` 的逐向量 compact record；stage1 是 32-vector FastScan batch，stage2 是候选向量逐个计算。
- SIMD-BP128 / FastPFOR 等固定 bit packing 论文强调整块吞吐，但其代价是随机访问少量元素时会解码无关元素。这个结论与当前 Stage1 剪枝后的 Stage2 场景一致：如果只需要一个 lane，就不应为了固定 batch 布局触碰其它 lane。
- bit-plane compression / bitmap 压缩适合存在大量全零 word 或 run 的 bitplane；上一轮 COCO100k `ex_bits=3` 统计显示 zero 64-bit plane word rate 只有约 6.90%，所以 sparse plane elision 的收益有限，尾部无效 lane 裁剪更有效。

参考：

- RaBitQ: Quantizing High-Dimensional Vectors with a Theoretical Error Bound for ANN Search, SIGMOD 2024 / arXiv:2405.12497
- The RaBitQ Library, OpenReview 2025
- RaBitQ-Library docs: Quantizer / IVF + RaBitQ / Compact Code Storage
- Daniel Lemire et al., Decoding billions of integers per second through vectorization
- Lemire, Boytsov, Kurz, SIMD Compression and the Intersection of Sorted Integers
- NVIDIA Research, Bit-Plane Compression

### 官方 RaBitQ-Library 对照

本地代码路径：`/home/zcq/VDB/third_party/RaBitQ-Library`

- `include/rabitqlib/quantization/data_layout.hpp`：
  `ExDataMap<T>::data_bytes(padded_dim, ex_bits)` 为 `padded_dim * ex_bits / 8 + 2 * sizeof(T)`，即逐向量 compact ex-code 加两个 factor。
- `include/rabitqlib/quantization/rabitq.hpp`：
  `quantize_split_batch()` 对 stage1 做 batch quantize，但 `for (i=0; i<num_points; ++i)` 逐向量写 ex-data。
- `include/rabitqlib/index/ivf/ivf.hpp`：
  `scan_one_batch()` 先对 32 个向量做 stage1 FastScan，再在 lower bound 通过时逐个调用 `split_distance_boosting()`。
- `include/rabitqlib/quantization/pack_excode.hpp`：
  `packing_2bit_excode()` / `packing_3bit_excode()` / `packing_4bit_excode()` 的输入是单个向量的 `uint8_t code[dim]`，输出是该向量自己的 compact byte buffer；代码注释里的 `vec_00_to_15` 指维度 0..15，不是向量 0..15。
- `include/rabitqlib/utils/space.hpp`：
  `ip64_fxu2_avx()` / `ip64_fxu3_avx()` / `ip16_fxu4_avx()` 都是单向量 ex-code 与 query 的 SIMD inner product，SIMD lane 用在维度方向，不跨多个候选向量。

这说明官方方案并没有把 stage2 组织成 8 个向量的 lane block；它牺牲一部分 stage2 batch 并行，换取剪枝后按候选访问的自然局部性。

更准确的对照是：

```text
官方 Stage1: 32-vector FastScan batch layout
官方 Stage2: per-vector compact bit-packed layout
当前旧 Stage2: 8-vector interleaved/lane layout
```

因此，本轮优化不应简单理解为“照搬官方 byte layout”，而应区分两层：

1. Stage2 存储粒度：官方是逐向量 record，更适合 Stage1 剪枝后的 survivor 访问。
2. Stage2 维度内 SIMD：官方仍然在单向量内部按 16/64 维块做 SIMD unpack 和 inner product。

## 当前实现问题

当前路径：

- 写入：`src/storage/cluster_store.cpp::ClusterStoreWriter::WriteVectors`
- 解析：`include/vdb/query/parsed_cluster.h::exrabitq_batch_block_view`
- 查询：`src/index/cluster_prober.cpp::ClusterProber::Probe`
- kernel：`src/simd/ip_exrabitq.cpp::IPOfficialRaBitQBatchCompactDirectBitplanesStridedMasked`

当前 direct bitplane v15 trimmed 布局为：

```text
block:
  uint32 valid_count
  for dim_block:
    for lane in valid_count:
      compact bitplanes for 64 dims
  factor_add[valid_count]
  factor_rescale[valid_count]
```

它已经支持 `lane_mask`，不会计算未请求 lane；但对单个候选向量而言，一个向量的 ex-code 被拆散到多个 `dim_block` 区域里。访问 lane 3 时需要在每个 dim block 里跳到 lane 3 的位置：

```text
db0 lane3 -> db1 lane3 -> db2 lane3 -> ...
```

因此 Stage1 剪枝越强，越容易出现两个问题：

- cache line 中相邻 lane 的数据被触碰但没有使用；
- 单向量跨 dim_block 跳转，不能一次连续读完整 ex-code。

## 候选路线

### 路线 A：适配所有 ex_bits 的通用逐向量布局

#### A1. `vector_bitplanes`

存储：

```text
block:
  uint32 valid_count
  for lane in valid_count:
    for dim_block:
      bitplanes[ex_bits][8B]   // ex_bits=1/2/3/4
  factor_add[valid_count]
  factor_rescale[valid_count]
```

查询：

- Stage1 收集到 `lane_mask` 后，只对请求 lane 调用逐向量 direct bitplane kernel。
- 每个 lane 的所有 dim_block 连续存放，cache 局部性最好。
- 支持 `ex_bits=1..4`，不改量化语义，recall 应完全一致。

预期：

- 单 lane 或低 survivor density 时速度提升。
- 存储大小与 trimmed bitplanes 持平。
- 对 all-lane 请求场景可能略慢，因为失去原本按 dim_block 共享 query load 的机会。

#### A2. `vector_bitplanes_prefetch`

在 A1 基础上加入轻量预取：

- 收集 Stage2 lane 后，按 `global_idx` 顺序访问 vector records。
- 对下一 lane 的 ex-code 和 factor 做 `__builtin_prefetch`。

预期：

- 对随机 survivor lane 和高维数据有更稳定的 p95。
- 代码风险低，但收益依赖 CPU 和 survivor 分布。

#### A3. `vector_bitplanes_microbatch`

在 A1 的逐向量存储格式上新增 survivor micro-batch 查询核：

- 磁盘和 resident memory 仍完全等同于 `vector_bitplanes`。
- 查询时先根据 `lane_mask` 收集实际 survivor lane，再按最多 4 个 survivor lane 为一组计算。
- 一个 micro-batch 内按 dim_block 遍历，复用 query load，并对同一维度块上的多个 survivor lane 分别累加 IP。
- SIMD 仍沿维度方向并行，micro-batch 只是减少重复 query load，并不改变 RaBitQ code 的语义。

实现状态：

- 已实现为 `vector_bitplanes_microbatch`。
- 支持 official `ex_bits=1..4`，并复用 `vector_bitplanes` 的文件格式和 resident storage。
- COCO100k `total_bits=4/ex_bits=3` 下，recall、文件大小、resident memory 和 rerank 数均不变，但 avg latency 比 `vector_bitplanes` 慢约 `1.5%`，Stage2 时间也更高。
- 当前结论：micro-batch 在这一轮没有成为有效 hot path。原因是 survivor 数量约 100/query，且每个 survivor 仍要读自己的 compact code；4-lane 分组节省的 query load 小于收集 lane、分组和多 accumulator 循环带来的额外开销。

### 路线 B：降低 lane packing 数量

#### B1. `small_batch4_bitplanes`

把 stage2 block size 从 8 降到 4：

```text
block_size = 4
layout = [dim_block][4 lanes][bitplanes]
```

预期：

- 比 8-lane 少一半无关 lane 触碰。
- 仍保留一定 batch 内共享 query load。
- 文件尾部 padding 更少，空间不大于 8-lane fixed，但通常不如 v15 trimmed。

风险：

- 当前 stage2 block 和 stage1 32-vector block 的映射假设较多，改全局 batch size 容易影响 `cluster_prober.cpp` 的本地 4 个 stage2 block scratch。

实现状态：

- 已实现为 `small_lane4_bitplanes`。
- 外层仍保留 8-vector block 与现有 factor/offset/header，只把 Stage2 payload 拆成两个最多 4-vector subgroup：
  `[subgroup4][dim_block][local_lane][bitplanes]`。
- 这样不改变文件大小和 resident memory，也避免触碰全局 batch-size 假设。
- COCO100k `total_bits=4/ex_bits=3` 下，avg latency 比 `vector_bitplanes` 低约 `2.7%`，recall 和存储/内存不变。
- COCO100k `total_bits=3/ex_bits=2` 下，avg latency 比 `vector_bitplanes` 慢约 `3.6%`；因此 small-lane4 暂时只适合作为 3-bit 路线候选。
- 后续第十一轮 10 次重复复核没有复现 early win：`small_lane4_bitplanes` 在 `ex_bits=3` 下比 `vector_bitplanes` 慢约 `1.86%`，因此不作为默认 hot path。

#### B2. `small_batch2_bitplanes`

把 stage2 block size 降到 2：

- 进一步减少无关 lane。
- 对低维或强剪枝场景可能更快。
- 对高 survivor density 可能退化，因为调用次数和 block metadata 增加。

实现状态：

- 已实现为 `small_lane2_bitplanes`。
- 与 `small_lane4_bitplanes` 一样，外层仍保留 8-vector block，只把 Stage2 payload 拆成 2-lane subgroup：
  `[subgroup2][dim_block][local_lane][bitplanes]`。
- COCO100k `total_bits=4/ex_bits=3` 下，recall、文件大小和 resident memory 不变，avg latency 比 `vector_bitplanes` 低约 `1.7%`。
- 但 `small_lane2_bitplanes` 比 `small_lane4_bitplanes` 慢约 `1.0%`，说明 2-lane 进一步降低 packing 数量后，subgroup/loop 开销超过了额外局部性收益。
- 后续第十一轮复核显示 `small_lane4_bitplanes` 本身也不稳定，因此 `small_lane2_bitplanes` 和 `small_lane4_bitplanes` 都不作为默认 hot path。

### 路线 C：bits-specialized direct IP

#### C1. `ex_bits=2` 专用 2-bit direct kernel

采用官方 2-bit compact 思路：每 64 维用 16B 存储，查询时以 16 维为单位 unpack：

- AVX512：加载 4B 得到 16 个 2-bit code，转 float 后 FMA。
- AVX2：按 8 维或 16 维拆开。

预期：

- 比通用 bitplane mask-add 少一次 plane 累加，适合 `total_bits=3`。
- 存储不变。

实现状态：

- 已实现为 `vector_2bit`。
- 采用 RaBitQ-Library 官方 2-bit compact block：每 64 维 16B，byte `j` 存第 `j`、`j+16`、`j+32`、`j+48` 维的四个 2-bit code。
- COCO100k `total_bits=3/ex_bits=2` 下，recall、文件大小和 resident memory 与 `vector_bitplanes` 完全一致。
- 低开销端到端 avg latency 为 `0.376670 ms`，略慢于 `vector_bitplanes` 的 `0.371782 ms`；因此该路线作为正确性验证和负例保留，不作为默认优化路径。

#### C2. `ex_bits=4` 专用 nibble direct kernel

每 64 维用 32B，两个 4-bit code 共 1 byte：

- AVX512/AVX2 用 shift/mask 解出低/高 nibble，再转 float FMA。
- 相比 bitplane 4 个 plane 的 mask-add，nibble unpack 更接近官方 4-bit compact storage。

预期：

- 对 `ex_bits=4` 的计算速度可能优于 bitplane 累加。
- 存储仍是 4 bit/dim，不降低 recall。

实现状态：

- 已实现为 `vector_nibble4`。
- 采用官方 4-bit compact group：每 16 维 8B，byte `j` 存第 `j` 维和第 `j+8` 维的两个 nibble。
- COCO100k `total_bits=5/ex_bits=4` 下，recall 与存储/内存不变，avg latency 相比 `vector_bitplanes` 下降约 5.4%，p95 下降约 8.0%。

### 路线 D：ex_bits=4 无损压缩

#### D1. `vector4_highplane_elide`

在 vector layout 内对每个向量的 4 个 bitplane word 做 per-plane nonzero mask，只写非零 plane word。

预期：

- 如果最高 bitplane 或低 bitplane 存在稀疏性，能接近 `ex_bits=3` 大小。
- 如果码值接近均匀分布，收益会很小，甚至因为 mask/rank 开销变大。

#### D2. `vector4_block_entropy`

对一批 vector records 做轻量无损块压缩：

- 按 4KB 或 16KB Stage2 payload block 做 LZ4/Zstd 类压缩；
- offset table 支持按 block 定位；
- 查询时只解压被访问的压缩 block。

预期：

- 对磁盘大小最有希望。
- 在线查询可能因解压放大尾延迟；除非 survivor 分布高度集中，否则不适合作为默认 hot path。

实现状态：

- 已实现为分析原型，而不是在线查询默认路径：
  `rabitq_code_zip/analyze_vector_bitplanes.py` 支持 `--compression-algorithms` 和 `--compression-block-sizes`。
- 已评估 `zlib1/zlib6/lzma0/lz4/zstd1/zstd3/zstd6`，block size 覆盖 `4KB/16KB/64KB`。
- 压缩对象只包含 Stage2 ex-code payload，并额外计入独立随机访问所需的 block offset table；这比压整段 `cluster.clu` 更符合在线查询访问模型。
- COCO100k 和 voxceleb2 的 4-bit 码值熵分别约为 `3.985` 和 `3.989` bit/dim。要接近 `ex_bits=3`，无损编码需要接近 `3.0` bit/dim；信息论下界已经不支持该目标。
- 实测最佳块压缩仍略大于原始 ex4 payload，并约为 ex3 payload 的 `1.334x`。因此 D2 也是负结果，不进入默认 hot path。

可行性边界：

- 如果 `ex_bits=4` 码值近似均匀，信息熵接近 4 bit/dim，无损压缩不可能稳定接近 `ex_bits=3` 的大小。
- 因此本轮应先做分布统计；若最高 bitplane 全零/稀疏不足，论文中应报告“无损压缩空间有限”，而不是强行引入高开销压缩。

## 本轮优先实现

1. 先实现 A1：`vector_bitplanes`，覆盖 `ex_bits=1..4`，作为逐向量局部性基线。
2. 再实现 C1/C2 的 kernel 分支：
   - `ex_bits=2`：2-bit direct compact IP。
   - `ex_bits=4`：nibble direct compact IP。
3. 如果 A1 在 `ex_bits=4` 上速度可接受，再追加 D1 的分布统计和压缩实验；D2 仅作为备选，不进入默认路径。

## 实验门槛

COCO100k 首轮对比：

- `nlist=2048`
- `nprobe=64`
- `query_count=1000`
- `topk=10`
- `non_safeout_candidate_budget=400`
- `dynamic_safeout=1`
- `dynamic_safein=static`

每个 bits 至少记录：

- R@10
- avg ms / QPS / p95
- avg rerank
- avg Stage2 candidates
- Stage2 kernel ms / collect ms / scatter ms
- cluster.clu bytes
- resident code bytes
- peak RSS KiB

判定：

- recall 与当前 direct/trimmed 基线一致。
- cluster.clu 与 resident code bytes 不高于对应 bit 的当前默认布局。
- avg ms 或 QPS 有可重复提升；如果只改善 p95，也单独记录。

## 最终路线选择

截至第十一轮复核：

| ex_bits | 默认路线 | 备选/说明 |
| ---: | --- | --- |
| 1 | `vector_bitplanes` | 与逐向量 bitplane layout 统一，保持最简单路径。 |
| 2 | `vector_bitplanes` | `vector_2bit` 和 `small_lane4_bitplanes` 在 COCO100k 上均未提速。 |
| 3 | `vector_bitplanes` | `vector_bitplanes_prefetch` avg 略低但差距小于噪声，作为可选变体；`small_lane4/small_lane2/microbatch` 均不进默认路径。 |
| 4 | `vector_nibble4` | 仅作为计算优化；sparse/elide 和块级熵压缩均不能接近 ex3 空间目标。 |

论文层面的表述建议：

- 主张成立的部分：逐向量 Stage2 layout 能减少跨 dim-block 访问和无关 lane 触碰，在 `ex_bits=3` 上相对旧 `split3_trimmed` 保持 recall/空间/内存不变并获得约 `1.5%` avg latency 改善。
- 不成立的部分：降低 lane 数量不是稳定收益；`small_lane4` 的 early win 无法在交错重复中复现。
- 边界分析：`ex_bits=4` 的无损空间压缩目标被信息熵和实际 codec 实验否定，应作为负结果或设计边界报告。

## 默认策略落地

截至第十二轮，代码默认行为与最终路线一致：

- `bench_build_index` / `bench_e2e` official 1+n 构建时，如果没有显式传 `--rabitq-exdata-layout`：
  - `ex_bits=1,2,3` 默认使用 `vector_bitplanes`；
  - `ex_bits=4` 默认使用 `vector_nibble4`；
  - `ex_bits=0` 保持 `generic_packed`。
- 显式传 `--rabitq-exdata-layout generic_packed` 时仍保留旧 generic 行为，用于兼容旧实验或诊断。
- `selected_direct` 现在解析为当前推荐 direct layout：
  - `ex_bits=1,2,3` -> `vector_bitplanes`；
  - `ex_bits=4` -> `vector_nibble4`。
- `ClusterStore` 写入时仍会把 `selected_direct` 转成实际 layout 后再落盘，不把伪布局写入 `cluster.clu`。

smoke 验证显示，不显式传 layout 时：

- official `total_bits=4/ex_bits=3` 生成 `index_official_1_plus_n_total4_ex3_vector_bitplanes`；
- official `total_bits=5/ex_bits=4` 生成 `index_official_1_plus_n_total5_ex4_vector_nibble4`。
