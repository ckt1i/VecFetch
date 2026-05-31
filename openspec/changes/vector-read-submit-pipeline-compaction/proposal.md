## Why

最新真实 GT benchmark 显示 `io_wait_ms ~= 0.002`，说明 resident/full-preload 模式下 I/O 等待已经被 CPU 覆盖；当前更大的瓶颈是每 query 约 `742` 个 `VEC_ONLY` read 在 `probe_submit` 路径中的 per-request CPU plumbing。上一轮 `AccumulateBlock` 优化没有证明端到端收益，因此下一轮应转向 vector read submit pipeline 的 CPU 侧压缩。

本 change 目标是在不改变 recall/top-k、I/O in-flight accounting、submit flush 语义和 fallback 行为的前提下，压缩 `VEC_ONLY` read plan 到 pending slot、io_uring prep、completion release 的热路径开销。

## What Changes

- 将 `VEC_ONLY` pending request emit 拆成独立 batch-oriented fast path：
  - `EmitVectorOnlyRequests`
  - `EmitAllRequests`
  - `EmitPendingDataRequests` 只保留预算分配、统计聚合和现有 flush 语义。
- 新增 query-local vec-only emit scratch，批量准备：
  - `AddressEntry`
  - buffer pointer
  - fixed-buffer index
  - pending slot id
  - fixed/fallback 标记
- 在 `AsyncIOSink` 的 resident single-assignment 主路径中减少 index indirection：
  - 允许 `SubmitScratch` 直接保存 `AddressEntry`。
  - 对 vec-only 和 safein-all 分别直接 append compact read plans。
- 进一步专用化 `VEC_ONLY` pending slot 和 release：
  - 避免 vector-only path 写入/清理不需要的 generic `PendingIO` 字段。
  - completion 继续准确归还 fixed vector buffer 或 vec-only pool buffer。
- 将 `fixed_vec_buffer_count=1024` 纳入本 change 的正式验证 sweep 和推荐实验口径，但默认值仍保持兼容。
- 保留现有 `VEC_ALL` / `PAYLOAD` / `CLUSTER_BLOCK` 通用路径语义。
- 使用真实 GT、`--early-stop 0`、分层 IVF baseline 验证端到端收益。

## Capabilities

### New Capabilities

无。该 change 是 resident query hot path 和 query pipeline 内部 submit-prep 边界的实现优化，不引入新的用户可见能力。

### Modified Capabilities

- `resident-query-hotpath`: 扩展 resident vector-only submit fast path，要求支持 batch-oriented emit、直接 address staging、专用 slot/release 和 fixed buffer count 验证。
- `query-pipeline`: 将 resident submit-prep 的 batch-first 边界具体化到 vec-only read submit pipeline，要求保持结果语义和 I/O accounting。

## Impact

- 影响代码：
  - `include/vdb/query/overlap_scheduler.h`
  - `src/query/overlap_scheduler.cpp`
  - `include/vdb/query/search_context.h`（如需补充轻量统计则仅追加字段，不删除旧字段）
  - `benchmarks/bench_e2e.cpp`（确认输出 fixed vec buffer 配置和已有统计）
  - `tests/query/overlap_scheduler_test.cpp`
  - 可能涉及 `tests/query/io_uring_reader_test.cpp` 或 fallback reader 测试
- 不改变：
  - 索引格式
  - candidate classification 语义
  - submit/poll/wait/final-drain 的 in-flight accounting
  - `VEC_ALL` / payload / cluster-block 生命周期
  - benchmark recall 口径和既有 JSON 字段名
- 风险：
  - pending slot 复用时遗漏字段覆盖会导致 completion 消费错误地址。
  - 直接 address staging 若处理不当可能破坏 dedup 或 safein-all 分类。
  - 过度融合到 `OnCandidates` 会破坏 submit budget / tail flush / stop-safe flush，因此本 change 不做 direct prep in `OnCandidates`。
