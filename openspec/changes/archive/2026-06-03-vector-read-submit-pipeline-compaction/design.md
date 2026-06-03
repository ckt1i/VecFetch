## Context

当前 MSMARCO `fht_kac_rotator` resident/full-preload、分层 IVF、真实 GT 工作点下，查询主要指标为：

- `avg_query_ms ~= 3.44`
- `probe_submit_ms ~= 0.88`
- `io_wait_ms ~= 0.002`
- `vec_only_reads ~= 742/query`
- `fixed_vec_buffer_hits/misses ~= 512/230`

这表明 I/O 等待已经不是瓶颈；`probe_submit` 中将大量 `VEC_ONLY` candidate 转成 read request 的 CPU plumbing 才是下一轮优化目标。现有代码已经具备 `VecOnlyReadPlan`、fixed vector buffer、vec-only pool、`AllocateVectorOnlyPendingSlot` 和 `ReleaseVectorOnlyPendingSlot`，但 emit 阶段仍然按 request 逐个执行 buffer 获取、slot 分配、io_uring prep 和统计更新。

本设计选择保留现有 pending queue 与 flush 语义，只压缩 queue-to-SQE 和 completion-release 的 CPU 路径，避免直接在 `OnCandidates` 中提交 I/O 导致 submit budget / tail flush / stop-safe flush 语义复杂化。

## Goals / Non-Goals

**Goals:**

- 降低 resident/full-preload 主路径中 `VEC_ONLY` read submit 的 per-request CPU 成本。
- 保持 `VEC_ALL`、`PAYLOAD`、`CLUSTER_BLOCK` 通用路径行为不变。
- 保持现有 submit budget、partial flush、tail flush、final drain 和 in-flight accounting。
- 保持真实 recall/top-k 语义不变。
- 让 fixed vector buffer count 的 1024 配置进入正式验证口径，用于减少 fallback miss。
- 为后续更激进的 SQE batch prep 或 direct submit 保留清晰边界。

**Non-Goals:**

- 不改变 Stage1/SafeOut/SafeIn 分类逻辑。
- 不减少 candidate 数量，不通过调 epsilon 或 nprobe 获得收益。
- 不做 rerank zero-copy 或 payload pipeline 重写。
- 不直接在 `AsyncIOSink::OnCandidates` 内 prep io_uring read。
- 不改变默认 `fixed_vec_buffer_count=0` 的兼容行为。
- 不删除现有 benchmark JSON 字段。

## Decisions

### Decision 1: 保留 pending queue，拆分 emit path

将 `EmitPendingDataRequests` 拆成：

```cpp
uint32_t EmitAllRequests(SearchContext& ctx, uint32_t max_count);
uint32_t EmitVectorOnlyRequests(SearchContext& ctx, uint32_t max_count);
```

外层继续负责：

- `max_count` submit budget
- all-read 优先级
- vec-only 剩余预算
- `probe_submit_emit_ms`
- `probe_submit_ms`
- `probe_time_ms`

理由：

- 保持现有 flush 语义和 in-flight accounting。
- 便于独立优化 vec-only path。
- 避免把 all/payload/cluster-block 通用路径卷入本 change。

备选方案是在 `OnCandidates` 里直接 prep read。该方案理论上可以少一次 pending queue，但会把 request generation 和 submit timing 强绑定，风险较高，本轮不采用。

### Decision 2: 为 vec-only emit 增加 query-local scratch

新增 scheduler-local scratch：

```cpp
struct VecOnlyEmitScratch {
    static constexpr uint32_t kMax = 1024;
    uint32_t count = 0;
    AddressEntry addrs[kMax];
    uint8_t* buffers[kMax];
    uint16_t fixed_indices[kMax];
    uint32_t slot_ids[kMax];
    bool fixed[kMax];
};
```

`EmitVectorOnlyRequests` 按批次执行：

1. 从 `pending_vec_only_plans_` 连续读取 address。
2. 批量 acquire fixed/vec-pool buffer。
3. 批量 allocate vector-only pending slot。
4. 批量 prep read。
5. 一次性更新 counters 和 head。

理由：

- 减少热循环中重复访问 queue/head/stats 的开销。
- 为后续 io_uring SQE batch prep 或 direct SQ ring 写入留出结构。
- scratch 是 scheduler-local，不引入 per-query allocation。

若 `max_count > kMax`，实现应分片处理，不得静默截断请求。

### Decision 3: AsyncIOSink 主路径直接 staged AddressEntry

当前 `SubmitScratch` 保存 `unique_indices`、`safein_all_indices`、`vec_only_indices`，`BuildReadPlans` 再通过多层 index 找回 `AddressEntry`。本 change 允许在 resident single-assignment 主路径直接保存：

```cpp
AddressEntry safein_all_addrs[kMax];
AddressEntry vec_only_addrs[kMax];
```

`ScanAndPartitionBatch` 在完成 dedup/分类后直接写 address，`BuildReadPlans` 或新的 append helper 直接 append compact read plans。

理由：

- 减少 indirection 和多次数组索引。
- 保持 dedup 和 classification 语义。
- 不改变 `CandidateBatch` 结构，也不影响非 resident path。

为了降低风险，第一版可以保留旧 index 数组字段，仅让 resident fast path 使用 direct address staging。旧字段可作为 fallback 或测试对照。

### Decision 4: 进一步专用化 vector-only slot/release，但不破坏 generic slot

现有 `PendingSlot` 继续服务所有请求类型。本 change 可以增加 vec-only 专用字段：

```cpp
AddressEntry vec_addr;
PendingIO::Type type;
```

`AllocateVectorOnlyPendingSlotFast` 只覆盖：

- `in_use`
- `buffer`
- `fixed_buffer_index`
- `cleanup`
- `type = VEC_ONLY`
- `vec_addr`

而不再写入完整 `PendingIO` 的 `read_offset/read_length` 等字段。`VEC_ALL`、`PAYLOAD`、`CLUSTER_BLOCK` 仍使用通用 `PendingIO`。

`ReleaseVectorOnlyPendingSlotFast` 只执行 vec-only 所需 release：

- `FixedVec` -> `ReleaseFixedVecBuffer(index)`
- `VecPool` -> `ReleaseVecOnlyBuffer(buffer)`
- 回收 slot id

理由：

- 减少 per-request store 和 cleanup。
- 避免通用 `slot.io = PendingIO{}` 清理成本。
- 保留 generic cleanup 给非 vector-only 和 shutdown cleanup。

关键约束：slot 复用时必须覆盖 `vec_addr`，completion user-data 到 slot id 的映射不变。

### Decision 5: fixed vector buffer count 默认兼容，验证推荐 1024

默认行为保持：

```cpp
fixed_vec_buffer_count == 0 -> io_queue_depth
```

正式 benchmark sweep 至少包含：

- `512`
- `1024`

主验证口径推荐使用 `1024`，因为当前 `512` 下仍有约 `230/query` fixed miss。

理由：

- 1024 个 768-dim float vector buffer 大约占 `1024 * 3072 ~= 3MB`，内存代价可控。
- 如果 fixed misses 接近 0，可以单独减少 vec-only pool fallback 和 cleanup 成本。

## Risks / Trade-offs

- [Risk] 直接 address staging 破坏 dedup 或 safein-all 分类。  
  Mitigation: 保留旧 index path 的语义边界，单测覆盖 duplicate、safein-all、vec-only 三类 candidate。

- [Risk] vec-only slot 不再清完整 `PendingIO` 导致复用脏字段。  
  Mitigation: completion dispatch 对 `VEC_ONLY` 只读取专用 `vec_addr` / buffer / cleanup；generic paths 继续使用 `PendingIO`。

- [Risk] batch emit 分片错误导致漏发或重复发。  
  Mitigation: 单测覆盖 partial flush、tail flush、`max_count < pending`、`max_count > scratch capacity`。

- [Risk] fixed buffer count 1024 改善有限。  
  Mitigation: 将其作为验证推荐，不改变默认；报告 fixed hit/miss 和 `probe_submit_ms`。

- [Risk] perf 仍被 CRC calibration 或 preload 污染。  
  Mitigation: benchmark 结论以真实 GT JSON 为准；perf 采用 delayed/query-focused 采样，并明确采样范围。
