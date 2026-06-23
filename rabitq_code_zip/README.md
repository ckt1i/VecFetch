# RaBitQ bit=4 stage2 无损压缩实验记录

本目录记录 `total_bits=4, stage1=1, ex_bits=3` 的 stage2 direct bitplane 压缩方案、实现入口和 COCO100k 结果。

## 目标

- 不改变量化码语义，不降低 recall。
- 在线查询端保持 direct bitplane dot，不把 stage2 全量解码成 `uint8_t[dim]`。
- 端到端平均查询延迟损失不超过 5%。
- 降低 `cluster.clu` 文件大小、resident preload 后的 index 内存和 online peak RSS。

## 调研结论

可选方案大致分为三类：

1. 固定位宽 bit-packing / BP128 / FastPFOR 类方案。相关工作强调固定 block 的 SIMD unpack 吞吐，例如 Lemire 等的 SIMD-BP128/FastPFOR 方向，以及 FastPFOR 工程实现。它适合整数列表压缩，但我们的 `ex_bits=3` 已经是 bitplane-packed 形态，再套一层 BP128 基本不能减少 3-bit payload。
2. StreamVByte / VByte 类变长整数压缩。StreamVByte 通过 control stream + data stream 提高 SIMD 解码速度，但更适合小整数或差分整数列表。COCO stage2 码值近似均匀，缺少变长编码收益，还会破坏当前按 lane 随机访问的 direct dot。
3. bitmap / bit-plane sparse elision。Roaring bitmap 等压缩 bitmap 工作说明当 0-run 或 sparse word 足够多时，bitmap 压缩能兼顾空间和速度。这个方向和我们的 bitplane word 布局最接近，因此实现了一个 sparse plane 对照。

参考资料：

- Daniel Lemire et al., "Decoding billions of integers per second through vectorization", arXiv:1209.2137, https://arxiv.org/abs/1209.2137
- Daniel Lemire and Leonid Boytsov, "SIMD Compression and the Intersection of Sorted Integers", https://r-libre.teluq.ca/601/1/simdcompressionarxiv.pdf
- Daniel Lemire et al., "Stream VByte: Faster Byte-Oriented Integer Compression", arXiv:1709.08990, https://arxiv.org/abs/1709.08990
- FastPFOR C++ library, https://github.com/fast-pack/FastPFOR
- Chambi et al., "Better bitmap performance with Roaring bitmaps", arXiv:1402.6407, https://arxiv.org/abs/1402.6407
- NVIDIA Research, "Bit-Plane Compression: Transforming Data for Better Compression in Many-core Architectures", https://research.nvidia.com/publication/2016-06_bit-plane-compression-transforming-data-better-compression-many-core

## COCO100k 分布检查

脚本：`analyze_direct3_bitplanes.py`

输入索引：

`/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_official_1_plus_n_total4_ex3_split3_bitplanes_eps0.90/cluster.clu`

输出：

`/home/zcq/VDB/test/rabitq_code_zip_20260606/direct3_bitplane_compression_estimate.csv`

关键统计：

- stage2 fixed ex bytes: 21,530,492
- zero 64-bit plane word rate: 6.90%
- 码值直方图接近均匀，0..7 均有大量出现
- sparse-position 编码估算反而变大
- trimmed valid lanes 估算约 93.14%
- zero-plane elision 估算约 94.89%

结论：bit=4 的压缩空间主要来自 batch 尾部 padding / 空 lane，而不是 stage2 码值自身的低熵。

## 实现的两种方案

### 1. `split3_trimmed_bitplanes`

存储格式版本：v15。

每个 batch block 只写 `valid_count` 个 lane：

- block header: `uint32_t valid_count`
- stage2 payload: `[dim_block][valid_count][3 planes x uint64_t]`
- factors: `valid_count` 个 `factor_add` + `valid_count` 个 `factor_rescale`

查询端不解码成 byte code，直接调用 strided bitplane dot，stride 从固定 8 lane 改成 `valid_count` lane。

### 2. `split3_zero_plane_elide`

存储格式版本：v15。

每个 dim block 对 3 个 bitplane 分别写：

- 1 byte nonzero lane mask
- 只写 mask 中非零 lane 的 `uint64_t` plane word

查询端按 mask 找到当前 lane 的 rank，把最多 3 个 `uint64_t` 临时放到 24B scratch，然后复用 direct bitplane dot。它验证了 sparse plane 思路，但由于 COCO zero word rate 不高，收益不如 trimmed。

## 文件格式入口

- `RaBitQExDataLayout::kSplit3TrimmedBitplanes`
- `RaBitQExDataLayout::kSplit3ZeroPlaneElide`
- CLI:
  - `--rabitq-exdata-layout split3_trimmed_bitplanes`
  - `--rabitq-exdata-layout split3_zero_plane_elide`

v15 variable ExRaBitQ region header：

- magic: `EXZ1`
- `uint32_t num_batch_blocks`
- `uint32_t offsets[num_batch_blocks + 1]`

offsets 相对 region start，便于 online query O(1) 找到 batch block。

## 与低 bit direct layout 的关系

`total_bits=2/3` 不需要走本轮 v15 变量块格式：

- `total_bits=2, ex_bits=1` 使用 `split1_bitplane`，stage2 是单个 bitplane。
- `total_bits=3, ex_bits=2` 使用 `split2_bitplanes`，stage2 是两个 bitplane。

也就是说，用户提出的 `1 + 1` 和单 stage2 bitplane 路径已经属于 direct layout 的自然特例。本轮新增的两个 layout 只服务 `total_bits=4, ex_bits=3`，因为这里才有 3 个 stage2 bitplane 且内存压力最高。

## 推荐

保留 `split3_trimmed_bitplanes` 作为 bit=4 stage2 压缩方案。它在 COCO100k 上不损失 recall，repeat 运行中延迟没有劣化，同时压低 `cluster.clu`、resident code storage 和 peak RSS。

`split3_zero_plane_elide` 可作为消融/负例保留：它说明当前数据分布下 sparse plane elision 的额外 rank/mask 解析成本不值得。
