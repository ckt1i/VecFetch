## 1. 存储格式与元数据

- [x] 1.1 增加新的 packed Stage2 magnitude cluster store version，并与当前 v10/v11 byte-magnitude layout 明确区分。
- [x] 1.2 扩展 cluster store metadata 和 parsed view，暴露当前 Stage2 magnitude bit width、packed magnitude block size、batch size、dim block、valid lane count、sign block 和 xipnorm 指针。
- [x] 1.3 保留现有 v10/v11 读取路径，确保旧 byte-magnitude 索引不会被误解释为 packed magnitude。
- [x] 1.4 增加 benchmark/index metadata 字段，用于标识 packed Stage2 magnitude format 和 storage version。

## 2. Bit-Pack Writer 与往返校验工具

- [x] 2.1 为 bits 2、4 实现 Stage2 magnitude code 的标量参考 pack/unpack 工具。
- [x] 2.2 在 writer 侧为 compact Stage2 batch block 实现 magnitude packing，同时不改变 Stage1 FastScan storage。
- [x] 2.3 增加 pack/unpack round-trip 单元测试，覆盖 bits 2/4、tail lanes 和包括 512 在内的代表性维度。
- [x] 2.4 增加 malformed/corrupt layout 检查，覆盖不支持的 bit width 和 packed block size 不匹配。

## 3. SIMD 解码 Scratch 路径

- [x] 3.1 增加可复用的 per-query 或 thread-local scratch buffer，容量覆盖一个被访问的 Stage2 compact batch block。
- [x] 3.2 为 bits=4 实现 nibble-level SIMD unpack，并在测试中与标量参考实现对拍。
- [x] 3.3 为 bits=2 实现 packed two-bit field SIMD unpack，并在测试中与标量参考实现对拍。
- [x] 3.4 明确 bits=3 暂不进入 packed Stage2 新格式；不支持的 bit width 应走旧格式或明确失败。
- [x] 3.5 确保 scratch allocation 位于被访问 block 的 inner loop 之外，并能跨 Stage2 evaluations 复用。

## 4. 查询与 Preload 集成

- [x] 4.1 更新 full `.clu` preload，使 packed Stage2 索引的 resident magnitude bytes 保持 packed，并跳过全索引 decoded magnitude mirror。
- [x] 4.2 更新 parsed cluster/resident view，使 Stage2 scheduling 能定位 packed magnitude blocks 并执行 touched-block decode。
- [x] 4.3 更新 Stage2 masked compact kernel call sites，只在 scoring 前对 touched packed blocks 解码。
- [x] 4.4 通过与 byte-magnitude reference score 对拍，保证 packed-decode Stage2 score 在浮点误差内保持一致。
- [x] 4.5 增加 metrics：packed Stage2 decode 是否启用、decode block count 或 timing、resident packed bytes、scratch capacity，并确保 `results.json` 暴露 scratch decode timing。

## 5. 验证与实验

- [x] 5.1 在 `/home/zcq/VDB/test` 下为 COCO100k 构建 bits 2、4 的 packed Stage2 新索引。
- [x] 5.2 在 COCO100k 上用 `nprobe=64`、`topk=10`、`non-safeout-candidate-budget=400` 与当前 byte-magnitude 索引做 recall/latency parity 检查。
- [x] 5.3 重新运行 COCO100k query memory profile，验证 peak RSS 相比当前 byte-magnitude bits 2/4 下降。
- [x] 5.4 测量 decode overhead 和端到端查询 latency，验证 bits=4 overhead 保持在设计目标内。
- [x] 5.5 将最终 memory、latency、index-size 和 recall 结果记录到 `/home/zcq/VDB/test`，并标注大数据集上的后续工作。
