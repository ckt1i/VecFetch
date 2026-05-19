## Why

现有 `stage2-kernel-sign-abs-optimization` 已经证明：仅把 Stage2 数据转成更规整的 resident parallel view 还不够，query-time kernel 仍然按 lane 独立做 `sign flip + abs unpack/convert + fmadd`，因此总工作量没有显著下降。现在需要把第二版方案推进到“compute-ready”层：不仅前移数据重排，还要把 query-time kernel 明确改成按 lane-batch 消费 `sign_pack/abs_pack` 的计算模型。

## What Changes

- 将现有 preload-time transcode 从“slice-friendly view”升级为“compute-ready view”，新增 `sign_pack`、`abs_pack` 一类 query-independent 前体表示。
- 新增 query-time 的 `sign_ctx` 概念，要求 kernel 先按 batch 生成 sign 上下文，再进入批量 `abs consume`，不再保留 lane-local 的主计算模型。
- 明确 resident view 与 query-time kernel 的分层：
  - preload-time 只负责构建 `sign_pack/abs_pack`
  - query-time 只负责把 `sign_pack + q_slice` 变成 `sign_ctx`，并按 lane batch 做 `sign apply + abs consume`
- 要求 Stage2 新 kernel 必须按 lane batch 处理，而不是只换地址布局后继续逐 lane 计算。
- 保留现有磁盘存储格式和 Stage2 语义，不引入索引重建。

## Capabilities

### New Capabilities

### Modified Capabilities

- `exrabitq-stage2-kernel`: 将第二阶段 resident view 与 query-time kernel 的要求升级为 compute-ready 设计，要求 resident 侧输出 `sign_pack/abs_pack` 前体，kernel 侧按 lane batch 构建 `sign_ctx` 并批量消费。

## Impact

- 影响代码：
  - `src/storage/cluster_store.cpp`
  - `include/vdb/query/parsed_cluster.h`
  - `include/vdb/storage/cluster_store.h`
  - `src/simd/ip_exrabitq.cpp`
  - `include/vdb/simd/ip_exrabitq.h`
  - `src/index/cluster_prober.cpp`
  - `benchmarks/bench_e2e.cpp`
- 影响系统：
  - resident/full-preload cluster build 路径
  - ExRaBitQ Stage2 resident view
  - Stage2 query-time SIMD kernel
  - Stage2 profiling 与 benchmark 归因
