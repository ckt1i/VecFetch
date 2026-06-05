## Why

当前 resident/full-preload 查询路径会把 `cluster.clu` 整体读入内存，同时还保留已解码地址、解析后的 per-cluster 视图等重复状态。这样会让在线查询内存统计显著高于真正必要的索引 resident footprint，也会掩盖 Stage2 bit-pack 后各 bit 配置的真实内存差异。

本次 change 的目标是在保持查询语义不变的前提下，把 preload 后的 resident index 改成只保留在线查询必需的信息，并用批量读取降低 compact preload 构建成本。

## What Changes

- 将 resident/full-preload 从“整文件常驻”改为“compact resident layout”：预加载阶段解析 `cluster.clu` 后，只保留查询所需的 code region、已解码地址表和必要的 ExRaBitQ parallel view。
- 在 preload 阶段预先解码 raw address block，并在 resident 状态中释放/不保留 raw address block，避免查询阶段重复解码地址。
- 修掉 decoded address 的重复持有：resident view 与 parsed cluster 不应各自拥有一份等价地址表。
- 将逐 cluster 临时 block 读取改为 16 个 cluster block 为一批的批量读取和批量解析，减少 preload/build resident view 的 syscall 与调度开销。
- 扩展 benchmark/RSS 输出，区分 `cluster.clu` 文件字节、实际 resident code bytes、decoded address bytes、parallel view bytes、raw-address retained bytes、重复 parsed-address bytes、preload/build resident view 用时和 query peak RSS。
- 在 COCO100k 上执行对比测试，至少覆盖现有 full-file preload 与 compact batched preload 两种路径，并报告时间构建成本和内存开销。
- 不改变 `data.dat` 原始向量读取语义；payload/raw-vector body 仍由现有查询 rerank 路径按需读取。

## Capabilities

### New Capabilities
- `compact-resident-cluster-preload`: 定义 compact resident preload 的内存保留语义、16-block 批量读取行为、地址预解码与去重要求。

### Modified Capabilities
- `clu-full-preload`: 将原来的整 `.clu` 文件常驻语义扩展为可选择的 compact resident preload，明确 compact 模式不得保留完整 file buffer。
- `e2e-benchmark`: 增加构建/preload 成本与 resident 内存拆分字段，支持 COCO100k 下同口径对比。

## Impact

- Affected code:
  - `include/vdb/storage/cluster_store.h`
  - `src/storage/cluster_store.cpp`
  - `include/vdb/query/parsed_cluster.h`
  - `benchmarks/bench_online_query.cpp`
  - `benchmarks/bench_e2e.cpp` or shared benchmark reporting helpers if present
  - `tests/storage/cluster_store_test.cpp`
- Runtime behavior:
  - resident/full-preload 查询不应再因为 `resident_file_buffer_` 保留完整 `.clu` 而高估在线索引内存。
  - compact preload 后查询路径应直接使用已解码地址，不再从 raw address block 逐批重复解码。
  - 16-block 批量读取只影响 preload/build resident view 成本，不改变 search top-k、recall 或 rerank 语义。
- Benchmark outputs:
  - COCO100k 测试结果需要能比较 build/preload wall time、RSS after preload、resident component bytes、query peak delta、recall@10 和 avg/p95/p99 latency。
