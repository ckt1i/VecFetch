## 1. Baseline And Scope Check

- [ ] 1.1 使用 code-review-graph 和源码确认 `OverlapScheduler` 中 `AsyncIOSink::OnCandidates`、`BuildReadPlans`、`EmitPendingDataRequests`、`AllocateVectorOnlyPendingSlot`、`ReleaseVectorOnlyPendingSlot` 的当前调用链。
- [ ] 1.2 记录当前主 baseline：真实 GT、`--early-stop 0`、two-level IVF、`fixed_vec_buffer_count=512` 下的 `avg_query_ms`、`probe_submit_ms`、`vec_only_reads`、`fixed_buf_hit/miss`、`recall@10`。
- [ ] 1.3 先用同一命令仅改 `--fixed-vec-buffer-count 1024` 跑一版验证，记录是否能降低 fixed misses 和 `probe_submit_ms`。

## 2. Split EmitPendingDataRequests

- [ ] 2.1 将 `EmitPendingDataRequests` 拆成 `EmitAllRequests(SearchContext&, uint32_t)` 和 `EmitVectorOnlyRequests(SearchContext&, uint32_t)` 两条内部路径。
- [ ] 2.2 保留外层 `EmitPendingDataRequests` 的 submit budget、all-read 优先级、vec-only 剩余预算、统计聚合和 `probe_time_ms` 更新逻辑。
- [ ] 2.3 确认 `VEC_ALL`、`PAYLOAD`、`CLUSTER_BLOCK` 的 buffer ownership、pending slot 和 cleanup 行为不变。
- [ ] 2.4 保留 `enable_hotpath_detailed_timing` 行为；关闭时 vector-only 热循环不得新增逐请求 `steady_clock::now()`。

## 3. Batch-Oriented Vector-Only Emit

- [ ] 3.1 在 `OverlapScheduler` 增加 scheduler-local `VecOnlyEmitScratch`，保存 address、buffer、fixed index、slot id、fixed/fallback 标记和 count。
- [ ] 3.2 `EmitVectorOnlyRequests` 从 `pending_vec_only_plans_` 连续读取 address，并按 scratch capacity 分片处理。
- [ ] 3.3 批量执行 fixed vector buffer acquire；fixed 不可用时回退 `AcquireVecOnlyBuffer`。
- [ ] 3.4 批量分配 vector-only pending slot，并一次性更新 `fixed_vec_buffer_hits`、`fixed_vec_buffer_misses`、`vec_only_read_requests` 和 `pending_vec_only_head_`。
- [ ] 3.5 批量 prep vector-only reads，优先使用 cached fixed-file + registered buffer path，不可用时保持现有 fallback。
- [ ] 3.6 确认分片处理不会在 `max_count < pending` 或 `pending > scratch_capacity` 时漏发或重复发。

## 4. Direct AddressEntry Staging

- [ ] 4.1 在 `SubmitScratch` 增加 `safein_all_addrs` 和 `vec_only_addrs`，保留旧 index arrays 作为 fallback 或对照。
- [ ] 4.2 在 resident single-assignment 主路径中，`ScanAndPartitionBatch` 完成 dedup/classification 后直接写入对应 `AddressEntry` staging。
- [ ] 4.3 更新 `BuildReadPlans` 或新增 append helper，直接从 staged addresses append `pending_all_plans_` 和 `pending_vec_only_plans_`。
- [ ] 4.4 保持 duplicate counters、`unique_fetch_candidates`、CRC estimate buffering 和 safein-all threshold 语义不变。
- [ ] 4.5 非 resident 或非 single-assignment fallback 继续可用，不能依赖 direct staging 才保证正确性。

## 5. Vector-Only Slot And Completion Release

- [ ] 5.1 在 `PendingSlot` 增加 vector-only fast path 所需的专用字段，例如 `vec_addr` 或 `type`，避免 vector-only path 依赖完整 generic `PendingIO`。
- [ ] 5.2 新增或收紧 `AllocateVectorOnlyPendingSlotFast`，只设置 vector completion 必需字段。
- [ ] 5.3 新增或收紧 `ReleaseVectorOnlyPendingSlotFast`，只归还 fixed vector buffer 或 vec-only pool buffer，并回收 slot id。
- [ ] 5.4 更新 `DispatchCompletion` 的 `VEC_ONLY` 分支，读取专用 address 字段并保持 `reranker.ConsumeVec(...)` 语义不变。
- [ ] 5.5 保留 `CleanupPendingSlots` 对异常/析构路径的安全清理能力，避免 buffer 泄漏。

## 6. Tests

- [ ] 6.1 更新或新增 `test_overlap_scheduler`：vector-only batch emit 在空队列、partial flush、tail flush、`max_count` 截断下无漏发/重复。
- [ ] 6.2 测试 `pending > VecOnlyEmitScratch::kMax` 时分片发射正确。
- [ ] 6.3 测试 fixed buffer hit 和 vec-only pool miss completion 后 buffer/free-list 正确恢复。
- [ ] 6.4 测试 direct address staging 下 duplicate、safein-all、vec-only 分类计数和 read request 数保持正确。
- [ ] 6.5 测试 non resident 或非 single-assignment fallback 路径仍可运行。
- [ ] 6.6 跑受影响回归：`test_overlap_scheduler`、`test_io_uring_reader`、`test_pread_fallback_reader`、`test_buffer_pool`、`test_rerank_consumer`、`test_ivf_index`。

## 7. Benchmark And Perf Validation

- [ ] 7.1 使用真实 GT、`--skip-gt 0`、`--early-stop 0`、MSMARCO `fht_kac_rotator`、two-level IVF baseline 跑 `fixed_vec_buffer_count=512`。
- [ ] 7.2 使用同一命令跑 `fixed_vec_buffer_count=1024`，比较 fixed miss、`probe_submit_ms` 和 `avg_query_ms`。
- [ ] 7.3 正式 optimized run 必须报告 `recall@1/5/10`、`avg_query_ms`、`probe_submit_ms`、`probe_submit_vec_only_emit_ms`、`io_wait_ms`、`vec_only_reads`、`fixed_buf_hit/miss`。
- [ ] 7.4 跑 delayed/query-focused perf，重点检查 `EmitPendingDataRequests`、`EmitVectorOnlyRequests`、slot allocation/release、buffer acquire/release 和 io_uring prep 采样变化。
- [ ] 7.5 如果 1024 fixed buffer 明显降低 misses 但不降低 latency，记录剩余瓶颈是否在 SQE prep、completion release 或 upstream candidate staging。

## 8. Reporting

- [ ] 8.1 在 change results 中记录完整 baseline 和 optimized 命令，明确使用真实 GT 而非 skip-GT。
- [ ] 8.2 报告是否达到目标：`probe_submit_ms` 从约 `0.88ms` 降到 `0.65~0.75ms`，`avg_query_ms` 降到 `3.20~3.30ms`。
- [ ] 8.3 若未达标，明确下一步应转向 Stage1 mask-first、SQE prep 内联化，还是 fixed-buffer/slot 之外的 submit 调度问题。
