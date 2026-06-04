## 背景与动机

当前 ExRaBitQ Stage2 的幅值码即使在 `rabitq_bits=2/3/4` 时，也仍然按每维一个 `uint8_t` 持久化。这会导致 bits 2/3/4 的 `.clu` 常驻内存几乎相同，削弱比特宽度扫描中关于量化向量内存效率的论证。

## 变更内容

- 将 Stage2 幅值码按当前 RabitQ 比特宽度以 bit-packed 形式写入 `cluster.clu`，同时保持现有 Stage2 数学语义不变。
- 完整预加载后，常驻的 cluster view 继续保持 packed 表示，不在预加载阶段物化全索引 `uint8_t` Stage2 幅值副本。
- 查询时只对 Stage2 实际访问的 compact block 临时解码到每次查询或每个线程复用的 scratch，然后进入 Stage2 kernel 计算。
- 为本轮目标比特宽度增加 SIMD 加速解包路径，聚焦 2-bit 和 4-bit；3-bit 暂不纳入本轮实现。
- 增加 COCO 下的基准测试和性能画像验证，覆盖 bits=2/4 的查询 RSS、索引大小、召回一致性和查询延迟开销。
- 新的 packed Stage2 存储版本要求重新构建索引；旧索引仍通过现有版本化 reader 路径读取。

## 能力项

### 新增能力项

无。

### 修改能力项

- `exrabitq-storage-layout`：新格式索引中，Stage2 `ex_code` 从每维一个 byte 改为按当前比特宽度 bit-pack 持久化。
- `ipexrabitq-compact-layout`：compact `v11` 的后继 Stage2 block layout 需要编码 packed magnitude，并暴露解码所需元数据。
- `exrabitq-stage2-kernel`：Stage2 查询路径需要支持把已访问 block 解码到 scratch，然后再进行 SIMD 幅值计算。
- `clu-full-preload`：常驻预加载必须保留 packed Stage2 bytes，避免物化全索引 decoded magnitude 副本。

## 影响范围

- 受影响代码：`src/storage/cluster_store.cpp`、`include/vdb/storage/cluster_store.h`、`include/vdb/query/parsed_cluster.h`、`src/simd/ip_exrabitq.cpp`、Stage2 调度/prober 路径，以及 `benchmarks/bench_e2e.cpp` 中的基准测试结果元数据。
- 受影响产物：`bits > 1` 时新构建的 `cluster.clu` 文件；已有索引应继续通过旧文件版本逻辑读取。
- COCO100k 预期收益：bits 2/4 的 Stage2 magnitude region 分别约减少 75%/50%；在常驻预加载下，查询 peak RSS 预计降低约 80-96 MiB。
- 预期代价：查询时增加 Stage2 解码开销；通过只解码已访问 block 和 SIMD 解包最小化该开销。COCO 端到端开销目标为不超过 20%，bits=4 优先争取低于 10%。
