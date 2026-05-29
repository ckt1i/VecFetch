## Why

MSMARCO768 `fht_kac_rotator` 的 resident/full-preload 查询路径仍然是 CPU bound：真实 recall 口径下 `io_wait ~= 0.002 ms/query`，而 `probe_submit ~= 1.0 ms/query`。当前每个 query 约有 745 个 vector-only read，其中大约 645 个 fixed-buffer miss 会落到通用 `BufferPool` 和通用 slot cleanup 路径，带来不必要的扫描、hash map 和分支开销。

## What Changes

- 为 resident `VEC_ONLY` fallback reads 增加固定尺寸的专用 buffer pool，避免通用 `BufferPool::Acquire/Release` 的 capacity 扫描和 `outstanding_` hash map。
- 为 `VEC_ONLY` completion 增加专用 slot release/cleanup fast path，只释放 fixed buffer 或 vec-only pool buffer，并复用 pending slot。
- 将 fixed vector registered buffer 数量从 `io_queue_depth` 中解耦，增加可配置的 fixed-buffer count，用于提高 fixed-buffer hit 率并支持 sweep。
- 保留通用 `BufferPool`、`VEC_ALL`、`PAYLOAD`、`CLUSTER_BLOCK` 和 pread/io_uring fallback 语义。
- 扩展 benchmark 配置和输出，确保后续性能结论必须带真实 recall，并导出 fixed-buffer count 与 vec-only pool 相关诊断。

## Capabilities

### New Capabilities

<!-- 无。此次 change 是对现有 resident 查询和 benchmark 能力的性能优化。 -->

### Modified Capabilities

- `resident-query-hotpath`: resident vector-only submit/completion path 增加固定尺寸 buffer 生命周期 fast path 和可调 fixed-buffer capacity。
- `benchmark-infra`: benchmark 验证要求使用真实 recall 口径，并支持 fixed vector buffer count sweep 与相关诊断输出。

## Impact

- 受影响代码：`src/query/overlap_scheduler.cpp`、`include/vdb/query/overlap_scheduler.h`、`include/vdb/query/search_context.h`、`benchmarks/bench_e2e.cpp`，以及相关 query tests。
- 受影响执行路径：resident/full-preload vector-only data read、pending slot cleanup、fixed registered buffer acquisition、benchmark/perf validation。
- 兼容性：不改变索引格式、不改变 top-k/recall 语义、不改变 payload 所有权，不移除通用 fallback 路径。
