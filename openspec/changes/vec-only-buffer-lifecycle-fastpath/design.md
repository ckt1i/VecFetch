## Context

当前 `fht_kac_rotator` resident/full-preload 查询路径已经是 CPU bound。真实 recall 口径下，`io_wait` 接近 0，而 `probe_submit` 约 1.0 ms/query。每个 query 大约有 745 个 `VEC_ONLY` data reads，其中 fixed registered buffer 命中约 100 个，剩余约 645 个走通用 `BufferPool`。

现有通用 `BufferPool` 需要按 capacity 扫描可用 buffer，并用 `outstanding_` hash map 记录归还时的容量。这个设计适合多尺寸 I/O buffer，但不适合 768 维 vector read 这种固定尺寸、高频、生命周期简单的路径。

## Goals / Non-Goals

**Goals:**

- 降低 resident `VEC_ONLY` fallback buffer acquire/release 的 CPU 成本。
- 降低 `VEC_ONLY` completion cleanup 的分支、hash lookup 和通用 slot reset 成本。
- 允许 fixed vector registered buffer 数量独立于 `io_queue_depth` 配置，以便通过 sweep 提高 fixed-buffer hit 率。
- 保持 recall/top-k、payload ownership、io_uring completion、shared/isolated reader 和 fallback 行为不变。
- 后续 benchmark 必须使用真实 recall 口径，避免把 `recall_available=false` 的 query-only 结果当成正确性结论。

**Non-Goals:**

- 不实现 rerank zero-copy。
- 不修改 `VEC_ALL`、`PAYLOAD` 或 `CLUSTER_BLOCK` 的通用生命周期。
- 不修改 Stage1/Stage2 classifier kernel 或 L2 rerank kernel。
- 不修改索引格式或构建流程。
- 不引入多线程查询执行。

## Decisions

### 决策：为 vec-only fallback reads 增加 scheduler-local fixed-size pool

`OverlapScheduler` SHALL 拥有一个只服务 `vec_bytes_` 的 vec-only pool。pool 维护 owned buffer 列表和 free-list，buffer 统一 4096-byte aligned，容量为 aligned `vec_bytes_`。

备选方案是继续扩展通用 `BufferPool` 的 size-class 支持。该方案可以复用组件，但仍需要处理多尺寸和 outstanding ownership，对当前单尺寸热点路径来说成本更高。

### 决策：新增 `VecPool` cleanup 类型，而不是复用 `Pool`

`PendingBufferCleanup` SHALL 增加 vec-only pool 专用 cleanup 类型。这样 completion 可以根据 slot cleanup 直接归还到 vec-only free-list，而不是进入 `BufferPool::Release()` 的 hash lookup。

备选方案是让 `BufferPool` 暴露无 hash release API。这样会污染通用 pool 的接口，并要求调用方记住 capacity，生命周期边界更模糊。

### 决策：为 `VEC_ONLY` completion 增加专用 release helper

`DispatchCompletion` 在处理 `VEC_ONLY` 后 SHALL 走专用 `ReleaseVectorOnlyPendingSlot(slot_id)` 或等价 helper。helper 只处理 fixed buffer / vec pool buffer 归还、必要 slot 字段清理和 free slot 回收。

备选方案是继续走 `CleanupPendingSlot()`。这更简单，但会保留当前最高频 completion 路径上的通用 switch 和不必要字段清理。

### 决策：fixed vector buffer count 独立配置，默认保持兼容

`SearchConfig` SHALL 增加 fixed vector buffer count 配置。默认值为 0 时沿用当前 `io_queue_depth` 行为；显式配置时使用该数量初始化 registered vector buffers。

备选方案是直接把 fixed buffers 固定扩大到 1024。这样可以快速提升 hit 率，但会隐藏不同平台的 registered-buffer 限制，也不利于找最优点。

### 决策：真实 recall 是性能结论的验收前提

后续正式 benchmark SHALL 使用 external GT 或 computed GT，使 `recall_available=true`。允许 query-only/perf 放大镜用于定位热点，但不得作为 recall 或最终速度结论的唯一依据。

备选方案是继续使用 `query-only + skip-gt`。这会让结果里的 recall 字段为 0，容易误读为算法退化，也无法证明优化未影响语义。

## Risks / Trade-offs

- buffer 生命周期回归 -> 保留通用路径并为 fixed buffer、vec pool buffer、slot reuse 增加聚焦测试。
- registered buffer 数量过大导致注册失败 -> 注册失败时回退到当前 fallback 行为，并通过 sweep 找平台可用范围。
- fixed buffer 增多但收益不明显 -> benchmark 输出 fixed-buffer hit/miss 和 vec pool acquire/release 指标，用数据判断是否保留默认值。
- 专用 fast path 增加 scheduler 复杂度 -> 只限定在 `VEC_ONLY`，不扩散到 all/payload/cluster block。
- perf 采样混入 preload 或 GT 计算 -> 正式 perf 使用 external GT 或 delayed record，并在报告中明确区分 query hot path 与 setup 阶段。

## Migration Plan

1. 增加 vec-only pool 数据结构和 cleanup 类型，但先保持旧路径可回退。
2. 将 `EmitPendingDataRequests` 的 fixed-buffer miss 路径切换到 vec-only pool。
3. 将 `VEC_ONLY` completion 切换到专用 release helper。
4. 增加 fixed vector buffer count 配置和 `bench_e2e` CLI/JSON 输出。
5. 增加 query scheduler 和 buffer lifecycle 单元测试。
6. 使用真实 recall benchmark 对比默认配置和 fixed-buffer sweep。
7. 若出现回归，关闭新配置或回退到通用 `BufferPool` 路径即可恢复旧行为。

## Open Questions

- fixed vector buffer count 的默认值是否只保持兼容，还是在验证后提升到 512/1024。
- 是否需要单独输出 vec-only pool hit/miss/acquire/release 统计，还是仅依赖 fixed-buffer miss 作为 proxy。
