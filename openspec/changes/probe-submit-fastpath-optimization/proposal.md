## Why

MSMARCO768 `fht_kac_rotator` 的 query-only perf 显示 resident/full-preload 查询路径已经是 CPU bound：`io_wait` 接近 0，而 `probe_submit` 每个 query 大约要 1.0-1.1 ms。当前最有价值的机会，是降低 vector-only submit 路径里的逐候选读取包装开销，以及紧邻的 completion-to-rerank 缓冲开销。

## What Changes

- 为 probe submit 路径增加低开销可观测性：vector-only/all/payload 请求计数、fixed-buffer 命中/未命中计数、slot 分配与 prep-read 计时，以及 rerank 向量分配/拷贝计时。
- 为 resident 查询执行引入 vector-only read-plan fast path，用紧凑的 ring/head-index 表示替代当前热点中的 `std::deque` 方案。
- 为 `VEC_ONLY` 读取增加轻量 pending-slot 路径，使热点请求不再承担完整通用 `PendingIO` 状态填充的成本。
- 为 io_uring fixed-file 读取缓存数据文件描述符的 registered index，避免每个请求都做 fd-index 查找。
- 用可复用的 per-query slab 替换逐候选 rerank 向量的 `aligned_alloc`，同时保留现有批量 rerank 语义。
- 在结构性开销降低之后，再重新跑 MSMARCO `fht_kac_rotator` query-only 的 perf/benchmark 验证，并调优 submit batching。

## Capabilities

### New Capabilities

<!-- 无。此次 change 是对现有 query pipeline 能力的性能优化。 -->

### Modified Capabilities

- `resident-query-hotpath`：在保留查询结果的前提下，降低 resident/full-preload 的 vector-only submit 与 completion 处理 CPU 开销。
- `batched-rerank-pipeline`：用可复用的 per-query 存储替换逐候选 rerank 向量分配，同时不改变 rerank 顺序或 collector 语义。
- `async-cluster-prefetch`：在保持 io_uring 异步读取语义不变的前提下，允许为热点数据读取准备缓存的 fixed-file 调用路径。
- `benchmark-infra`：扩展 benchmark/perf 可观测性，用于 submit 路径诊断和回归验证。

## Impact

- 受影响的代码：`src/query/overlap_scheduler.cpp`、`include/vdb/query/overlap_scheduler.h`、`src/query/io_uring_reader.cpp`、`include/vdb/query/async_reader.h`、`src/query/rerank_consumer.cpp`、`include/vdb/query/rerank_consumer.h`，以及 `SearchStats` 的 benchmark 输出。
- 受影响的执行路径：resident/full-preload 查询模式、vector-only 数据读提交、pending-slot 生命周期、io_uring SQE 准备、completion dispatch，以及批量 rerank buffering。
- 兼容性：不应改变对外 CLI/API 行为。召回率、结果排序、payload 所有权和异步读正确性都必须保持不变。
