## Context

现有 pipeline 消融通过 `serial_data_drains` 在每个 cluster 后立即 drain data I/O。该实现能减少跨 cluster overlap，但仍然保留 `io_uring`、queue depth、batch submit、completion dispatch 和批量 rerank。因此它不能作为“完全串行 No Pipeline”强对照。

本 change 的目标是给论文 pipeline 消融提供一个主口径：在复用同一 RecordGate resident 索引和同一候选策略的前提下，把查询执行拆成严格阶段：

1. coarse routing；
2. probe + RaBitQ/SafeOut/SafeIn/candidate collection；
3. 串行 raw-vector 或 full-record 读取；
4. exact rerank；
5. final top-k payload 串行读取；
6. result assembly。

该路径用于回答：如果没有 fetch-aware overlapped execution，访问动作是否会产生可观串行等待或尾延迟。

## Goals / Non-Goals

**Goals:**

- 增加 `serial_no_overlap` 查询执行模式，默认路径仍为现有 overlap pipeline。
- 复用现有索引、resident cluster parser、coarse routing、RaBitQ estimator、SafeOut/SafeIn、candidate budget 和 final result 语义。
- 在 probe 阶段只收集 read plans，不提交任何 raw-vector 或 payload I/O。
- probe 完成后串行执行 read plans；raw vector/full record 读取必须通过同步 `pread` 或等价同步接口完成。
- 保留 exact rerank 的 batch SIMD 计算能力，避免把 pipeline 消融混入 rerank compute 消融。
- 输出足够统计字段来证明 Full Pipeline 和 Serial NoOverlap 的候选、读取、rerank 和 recall 口径匹配。

**Non-Goals:**

- 不实现新的 IVF+RaBitQ+FlatStor baseline。
- 不重建索引，不改变 RecordGate 物理格式。
- 不改变 SafeOut/SafeIn 判定逻辑、epsilon、two-level coarse routing、candidate budget 或 top-k 语义。
- 不把 `serial_data_drains` 旧口径删除；它可作为弱诊断保留，但不能继续命名为强 No Pipeline。
- 不用本 change 优化串行路径性能；串行路径主要用于消融归因。

## Decisions

### Decision 1: 在同一 scheduler 内增加执行模式，而不是单独写 baseline runner

实现新增 `QueryExecutionMode`，至少包含：

- `Overlap`：现有默认路径。
- `SerialNoOverlap`：新增完全串行 No Pipeline 主口径。

`bench_online_query` 增加 `--execution-mode overlap|serial-no-overlap`，并在 JSON 中写出执行模式。现有 `--serial-data-drain` 保留为 lower-level diagnostic flag，但当 `execution-mode=serial-no-overlap` 时不应依赖它实现语义。

Alternative considered: 新建独立 IVF+RaBitQ+FlatStor runner。  
Rejected because 它会改变 baseline family、payload store 和访问路径，不能隔离 pipeline 贡献。

### Decision 2: 新增 collecting sink，复用 ClusterProber 候选输出

`ClusterProber::Probe()` 已经通过 `ProbeResultSink` 输出 non-SafeOut candidates。新增 `CollectingIOSink` 或把当前 `AsyncIOSink` 中的候选扫描/分类逻辑抽出为共享 helper：

- 保持 batch 内 dedup 和全局 dedup 行为。
- 保持 SafeIn/Uncertain 分类统计。
- 保持 dynamic SafeOut frontier estimate buffering / merging。
- 保持 dynamic SafeIn frontier state 更新。
- 对每个 surviving candidate 生成与 overlap path 相同的 `ReadPlanEntry` 或 `VecOnlyReadPlan`。
- 不调用 `PrepRead`、`Submit`、`Poll` 或 `WaitAndPoll`。

Alternative considered: 复用现有 `AsyncIOSink` 并设置 queue depth 为 1。  
Rejected because queue depth=1 仍然通过 async submission/completion path 执行，且不保证 probe、read、rerank、payload 阶段完全分离。

### Decision 3: 串行路径保留 batch rerank，但关闭 I/O overlap

串行路径在 probe 完成后对 materialized read plans 做同步读取：

- `VEC_ONLY`：同步读取 raw vector bytes，传给 `RerankConsumer::ConsumeVec()` 或等价 owned-buffer 接口。
- `VEC_ALL`：同步读取完整 record，传给 `RerankConsumer::ConsumeAll()`，保留 SafeIn full-record payload cache 语义。
- `PAYLOAD`：final top-k 后同步读取缺失 payload，传给 `RerankConsumer::ConsumePayload()` 或直接 cache/parse。

读取全部完成后再调用 `RerankConsumer::ExecuteBuffered()`，继续使用现有 batch SIMD exact rerank。这样比较只隔离 I/O scheduling/overlap，而不把 batch rerank 改成逐向量 scalar。

Alternative considered: 每读一个 vector 立即 scalar rerank。  
Rejected because 这同时关闭 batch rerank/SIMD，会把计算路径差异混入 pipeline 消融。

### Decision 4: candidate budget 和 SafeIn full-record policy 必须在串行路径中复刻

如果启用 `non_safeout_candidate_budget`，串行路径必须先按照与 overlap path 相同的 rank key 维护 top-B read plan heap，再 materialize 最终 read plans。`budgeted_prefetch_limit` 在串行模式下必须为 0 或被忽略并显式记录，因为 speculative prefetch 与 Serial NoOverlap 语义冲突。

SafeIn-as-full-record 的访问动作也必须保留：如果 Full Pipeline 会把某个 candidate 作为 `VEC_ALL` full record 读取，Serial NoOverlap 应在串行 read 阶段读取相同字节范围。否则比较会同时消融 SafeIn prefetch/access policy。

Alternative considered: 串行路径全部降级为 vector-only + final payload。  
Rejected for主口径，因为它对应 LateMaterialization 变体，而不是 pure pipeline ablation。

### Decision 5: benchmark 必须有强归因字段

`bench_online_query` 输出至少包含：

- `execution_mode`
- `serial_no_overlap`
- `async_io_enabled`
- `serial_vector_read_ms`
- `serial_full_record_read_ms`
- `serial_payload_read_ms`
- `avg_serial_vector_read_requests`
- `avg_serial_full_record_read_requests`
- `avg_serial_payload_read_requests`
- existing `avg_total_probed`
- existing SafeOut/SafeIn/Uncertain counts
- existing rerank count and read bytes

Full Pipeline 与 Serial NoOverlap 结果只有在 recall、probed count、candidate count、read count、rerank count 匹配时，才允许用于 pipeline claim。

## Risks / Trade-offs

- [Risk] 串行路径不小心改变 candidate 选择，导致性能差异无法归因。  
  Mitigation: 在结果汇总脚本中强制比较 `avg_total_probed`、SafeOut/SafeIn/Uncertain、rerank count、read requests/bytes；不匹配的点标记为 invalid。

- [Risk] 保留 batch rerank 会被误解为“不完全串行”。  
  Mitigation: 文档中明确该口径是 `No I/O overlap`，而不是 `No SIMD`。如果论文需要，可追加 appendix 的 strict scalar 诊断，但不作为主口径。

- [Risk] 同步读取 full record 可能使用不同 buffer ownership，导致 payload cache 或 assembly bug。  
  Mitigation: 优先复用 `RerankConsumer::ConsumeVec/ConsumeAll/ConsumePayload`，只替换读的调度方式。

- [Risk] `budgeted_prefetch_limit` 与串行模式冲突。  
  Mitigation: 串行模式下强制忽略 speculative prefetch，并在 benchmark 输出中记录 effective value 为 0。

- [Risk] 当前 `AsyncIOSink` 内部包含大量 frontier/dedup/read-plan 逻辑，直接复制会形成双份维护。  
  Mitigation: apply 阶段优先抽取共享 candidate-plan helper；若时间有限，允许先实现小范围重复，但 tasks 中必须包含后续整理和测试。

## Migration Plan

1. 增加 `QueryExecutionMode` 和 CLI `--execution-mode`，默认保持 `overlap`。
2. 抽取或新增 collecting sink，用于 probe 阶段只收集 read plans。
3. 增加 `SearchSerialNoOverlap()` 或在 `Search()` 内按 mode 分派。
4. 增加同步 read-plan materialization 和串行 read helpers。
5. 增加 benchmark JSON/CSV 字段与 summary 脚本支持。
6. 用已有索引跑 smoke：COCO topk=10,nprobe=64,queries=100。
7. 用 pipeline 消融选定点跑 Full vs Serial NoOverlap，验证候选/read/rerank 计数一致。
8. 如果计数不一致，先修语义，不把该结果用于论文。

## Open Questions

- 是否保留一个 appendix-only `serial-late-materialization` 变体，用于解释与 IVF+RaBitQ+FlatStor 的关系；本 change 不默认实现。
- 串行路径是否需要支持 `separate_record_store` NoCombine；主实验不要求，但 NoCombine 复用时可能有价值。
