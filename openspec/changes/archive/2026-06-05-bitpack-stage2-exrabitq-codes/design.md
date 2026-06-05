## 背景

Stage1 FastScan code 已经采用面向 SIMD 的 block-32 bit-packed layout。当前内存低效点在 ExRaBitQ Stage2 magnitude：对于 `bits > 1`，`ex_code` 被编码成每维一个 `uint8_t`，当前 compact `v11` Stage2 layout 也会把这种每维一个 byte 的表示写入 `cluster.clu`。

在 COCO100k 的 512 维向量上，当前 Stage2 magnitude payload 不管 `rabitq_bits` 是 2、3 还是 4，每个向量都占 512 bytes。常驻预加载还会构建完整 decoded `exrabitq_parallel_abs_blocks_storage` view，使另一份每维一个 byte 的 Stage2 表示常驻内存。这就是 bits 2/3/4 的查询 RSS 几乎相同的根本原因。本轮实现只覆盖 bits=2 和 bits=4；bits=3 暂不做 packed/SIMD 路径。

## 目标 / 非目标

**目标：**

- 为 `bits > 1` 引入需要重新构建索引的 packed Stage2 存储格式。
- Stage2 magnitude code 按实际比特宽度持久化，而不是每维一个 byte。
- 常驻预加载状态保持 packed，避免物化全索引 decoded magnitude 副本。
- 查询执行期间只对 Stage2 实际访问的 batch block 临时解码到 scratch。
- 为 bits=2 和 bits=4 提供 SIMD 解包路径，降低解码开销。
- 相对当前 byte-magnitude Stage2 实现保持 recall/search 语义不变。
- 通过现有版本化解析路径继续读取旧 v10/v11 索引。

**非目标：**

- 不改变 Stage1 FastScan 存储；它已经是 bit-packed。
- 不改变 RabitQ 量化数学、阈值、SafeOut/SafeIn 策略或 candidate budgeting。
- 不承诺旧二进制可以读取新的 packed Stage2 格式。
- 不增加全局 decoded Stage2 cache；这会抵消大部分 RSS 收益。

## 设计决策

### 决策 1：为 packed magnitude 使用新的 `cluster.clu` 存储版本

新布局作为当前 compact `v11` 的后继，在本文档中称为 `v12`。

理由：现有规格和代码都把 v11 视为 compact blocked layout，但 magnitude 仍是 `uint8`。复用 v11 会让旧 v11 文件语义不清，存在静默误解析风险。新版本可以明确重新构建索引和回滚的边界。

备选方案：基于元数据标记重新解释 v11。该方案被拒绝，因为 `cluster.clu` 解析本来就是显式版本化的，隐藏标记会增加兼容复杂度。

### 决策 2：只 pack Stage2 magnitude，sign 和 `xipnorm` 保持不变

持久化后的 v12 Stage2 batch block 保留：

- `valid_count`
- 使用 `rabitq_bits` bit-pack 后的 magnitude bytes
- packed sign bytes
- `xipnorm[8]`

以 `dim=512` 为例，每个向量的 Stage2 payload 大致从：

```text
当前 v11:
  abs:     512 bytes
  sign:     64 bytes
  xipnorm:   4 bytes

v12 packed magnitude:
  bits=2 abs: 128 bytes
  bits=4 abs: 256 bytes
  sign:       64 bytes
  xipnorm:     4 bytes
```

理由：sign 已经 packed；`xipnorm` 是单个 scalar，不值得压缩；magnitude 是主要可节省内存来源。

### 决策 3：resident preload 保持 packed

`PreloadAllClusters()` 继续把完整 `.clu` 读入 `resident_file_buffer_`，但 v12 的 resident parsed view 必须指向 packed magnitude block，不得为整个索引构建 `exrabitq_parallel_abs_blocks_storage`。

理由：COCO 的内存收益依赖同时减少 `.clu` 常驻 bytes 和当前 decoded parallel view。预加载阶段全量 decode 虽能省磁盘空间，但无法显著降低查询 RSS。

备选方案：预加载时一次性 decode v12，以复用现有 kernel。该方案不作为主路径，因为它基本保留当前 peak RSS。

### 决策 4：只对 touched Stage2 batch block 解码到 scratch

当 Stage2 调度决定某个 batch block 需要评估时，查询路径将该 block 的 packed magnitude 解码到 thread-local 或 per-query scratch，形成与当前 v11 lane-major shape 兼容的临时缓冲，然后调用现有 compact Stage2 SIMD kernel 或其轻量封装。

```text
常驻 v12 block
   packed_abs + sign + xipnorm
          |
          | 仅当 Stage2 访问该 block 时执行
          v
线程本地 scratch
   decoded_abs[8 lanes][dim]
          |
          v
现有或轻量封装的 IPExRaBitQ compact kernel
```

理由：Stage2 只访问候选 block 的子集。COCO `nprobe=48, budget=400` 下，bits 2-4 当前每个 query 大约触发 249-294 次 masked Stage2 kernel call。只解码已访问 block 可以把解码开销限制在实际 Stage2 workload 上。

### 决策 5：SIMD 解码 + 标量兜底

提供以下 unpack 实现：

- 2-bit：基于 byte/nibble extraction，使用 AVX2/AVX-512 widening 和 mask。
- 4-bit：nibble 解包，天然是每个 byte 两个 code。
- 3-bit：本轮不实现 packed 路径；若后续需要，再作为单独任务补充。

理由：bits=4 很可能继续作为后续实验默认点，因此需要优先优化它的解码路径。标量兜底有助于可移植性和测试。

### 决策 6：通过存储、正确性和端到端性能画像三层验证

该变更需要在三个层面验证：

- 单元和布局测试：bits 2/4 的 pack/unpack round-trip，覆盖 tail lanes 和代表性维度。
- 查询正确性：Stage2 score 和最终结果与当前 byte-magnitude 格式在浮点误差内一致。
- 端到端性能画像：COCO100k 查询 RSS 降低，同时 latency overhead 受控。

## 风险 / 取舍

- Packed decode 会增加热路径 CPU 工作量：通过只解码已访问 block、SIMD 解包、scratch 复用和分 bit 微基准测试缓解。
- 3-bit 暂不支持新 packed format，可能导致 bits=3 仍走旧 v11 路径：通过 metadata 和不支持 bit width 的检查明确边界。
- 新文件版本需要重新构建索引：在 benchmark metadata 中明确重建要求，并保留 v10/v11 旧 reader 路径。
- Scratch allocation 可能引入开销：scratch 在 worker/query context 中预分配并跨 touched blocks 复用。
- 如果实现时意外全量 decode 常驻 blocks，内存收益会消失：增加 resident packed bytes、decoded scratch bytes 相关 metrics，并追踪 v12 不产生全索引 decoded resident magnitude buffer。

## 迁移计划

1. 增加 v12 writer 和 reader 支持，同时保留 v10/v11 解析。
2. 为目标数据集构建新的 v12 索引；不原地修改已有索引。
3. 运行 pack/unpack 和 query parity tests。
4. 重新运行 COCO memory profile 和端到端 latency 对比，覆盖 bits 2/4。
5. 回滚路径：继续使用现有 v11 索引和旧 reader 行为；旧二进制不承诺读取 v12 索引。

## 开放问题

- 初始上线是否需要把 v11 保持为默认构建格式，并通过 flag 启用 v12；验证完成后再切换默认。
- 3-bit 是否在后续单独实现 packed 路径。
- `results.json` 需要暴露 scratch decode timing；是否额外暴露 packed Stage2 bytes 可根据实现复杂度决定。
