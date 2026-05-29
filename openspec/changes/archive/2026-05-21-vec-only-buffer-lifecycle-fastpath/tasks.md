## 1. Vec-Only Buffer Pool

- [x] 1.1 在 `OverlapScheduler` 中增加 scheduler-local vec-only pool 状态，包含 owned buffer 列表、free-list、aligned vector buffer size 和析构释放逻辑。
- [x] 1.2 实现 `AcquireVecOnlyBuffer()` 和 `ReleaseVecOnlyBuffer()` helper，确保 buffer 4096-byte aligned 且容量覆盖 `vec_bytes_`。
- [x] 1.3 将 vec-only pool 初始化与 query 生命周期解耦，允许跨 query 复用容量，并在 scheduler 析构时释放所有 owned buffers。
- [x] 1.4 保留通用 `BufferPool` 给 `VEC_ALL`、`PAYLOAD` 和其他非 vec-only fallback 路径使用。

## 2. Vector-Only Completion Release Fast Path

- [x] 2.1 扩展 `PendingBufferCleanup`，增加 vec-only pool 专用 cleanup 类型。
- [x] 2.2 将 `EmitPendingDataRequests` 中 fixed-buffer miss 的 `VEC_ONLY` read 切换为 vec-only pool buffer，并设置对应 cleanup 类型。
- [x] 2.3 增加 `ReleaseVectorOnlyPendingSlot(slot_id)` 或等价 helper，处理 fixed buffer 归还、vec-only pool buffer 归还和 pending slot 复用。
- [x] 2.4 将 `DispatchCompletion` 的 `VEC_ONLY` 分支切换到专用 release helper，保持 `RerankConsumer::ConsumeVec` 语义不变。
- [x] 2.5 确认 `VEC_ALL`、`PAYLOAD`、`CLUSTER_BLOCK` 和 final cleanup 仍走原有所有权路径。

## 3. Fixed Vector Buffer Count Configuration

- [x] 3.1 在 `SearchConfig` 增加 fixed vector buffer count 配置，默认值为 0，表示沿用当前 `io_queue_depth` 行为。
- [x] 3.2 修改 `InitializeDataBufferSlab()`，使用显式 fixed vector buffer count 或 fallback 到 `io_queue_depth` 初始化 registered vector buffers。
- [x] 3.3 当 fixed buffer allocation 或 registration 失败时，清理已分配 buffers，并保持 fallback 读路径可用。
- [x] 3.4 在 `bench_e2e` 增加 `--fixed-vec-buffer-count` CLI 参数，并在 config JSON 中输出实际配置值。

## 4. Observability And Tests

- [x] 4.1 复用现有 fixed-buffer hit/miss 和 read request 统计，必要时增加 vec-only pool acquire/release 计数用于验证生命周期路径。
- [x] 4.2 增加或更新 query scheduler 单元测试，覆盖 fixed-buffer miss 使用 vec-only pool、completion 后归还 pool、跨 query 复用和无重复释放。
- [x] 4.3 增加 fixed vector buffer count 配置测试，覆盖默认兼容、显式 count 和 registration 不可用 fallback。
- [x] 4.4 运行 `test_overlap_scheduler`、`test_rerank_consumer`、相关 async-reader / buffer-pool 测试和 `bench_e2e` smoke。

## 5. Real-Recall Benchmark And Perf Validation

- [x] 5.1 使用 `/home/zcq/VDB/test/data/MSMARCO/fht_kac_rotator` 索引和真实 ground truth 运行 resident/full-preload benchmark，确保 `recall_available=true`。
- [x] 5.2 对 fixed vector buffer count 执行 sweep，至少覆盖默认、128、256、512、1024，记录 `avg_query_time_ms`、`probe_submit_ms`、`probe_submit_vec_only_emit_ms`、fixed-buffer hit/miss 和 recall@10。
- [x] 5.3 对最佳配置运行 delayed `perf record`，确认 `BufferPool::Acquire`、`CleanupPendingSlot`、allocator 路径采样下降。
- [x] 5.4 验证 `io_wait_ms` 仍接近 0，recall@10 与基线在同一 query 集和同一 ground truth 下保持稳定。
- [x] 5.5 将 benchmark 输出路径、关键指标和推荐 fixed buffer count 记录到 change 结果说明中。
