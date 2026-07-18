## Why

当前 pipeline 消融只通过 `--serial-data-drain 1` 近似破坏跨 cluster 的 I/O overlap，但仍保留 `io_uring`、queue depth、批量提交和 completion 后的批量 rerank。这个口径不足以支撑“Fetch-aware pipeline 是独立贡献”的强 claim，需要一个复用同一 RecordGate 索引、只关闭执行重叠的完全串行 No Pipeline 主口径。

## What Changes

- 新增一个 benchmark-only 查询执行模式，例如 `serial_no_overlap`。
- 该模式复用现有 resident RecordGate 索引、two-level coarse routing、RaBitQ Stage1/Stage2、SafeOut/SafeIn 判定、candidate budget、dedup 和 final top-k 语义。
- 该模式在 probe 阶段只收集需要验证的候选，不发起异步 raw-vector 或 payload I/O。
- probe 完成后，按收集到的候选串行读取原始向量，执行与主路径等价的 exact rerank。
- rerank 得到最终 top-k 后，再串行读取缺失 payload，完成结果组装。
- 保留现有 overlap pipeline 作为默认路径；新模式只作为 pipeline 消融和 reviewer-facing 诊断口径使用。
- benchmark 输出新增或明确标记执行模式、串行 raw-vector read 时间、串行 payload read 时间、candidate/read count、是否启用异步 I/O、是否存在 in-flight overlap。
- 不把该模式改造成新的 IVF+RaBitQ+FlatStor baseline；它是 RecordGate 同索引、同候选策略下的执行调度消融。

## Capabilities

### New Capabilities

- None.

### Modified Capabilities

- `query-pipeline`: 增加同索引、同候选语义下的 `serial_no_overlap` 查询执行模式，用于完全串行 No Pipeline 消融。
- `e2e-benchmark`: 增加执行模式选择、串行模式元数据和分段统计输出要求，使 Full Pipeline 与 Serial NoOverlap 能在同口径下比较。

## Impact

- Affected code:
  - Query configuration and stats: `include/vdb/query/search_context.h`
  - Query scheduling: `include/vdb/query/overlap_scheduler.h`, `src/query/overlap_scheduler.cpp`
  - Candidate collection sink and serial read/rerank helpers, likely in `src/query/overlap_scheduler.cpp` or a small new query helper.
  - Benchmark CLI and JSON/CSV output: `benchmarks/bench_online_query.cpp`
  - Summary scripts for pipeline ablation under `paper/experiment-documents/recordgate-pipeline-ablation-20260701/`
- Compatibility:
  - Default overlap behavior remains unchanged.
  - Existing indexes remain readable and no rebuild is required.
  - Existing `--serial-data-drain` can remain as a weaker diagnostic flag, but must not be labeled as the strong No Pipeline baseline.
- Validation:
  - Same-index Full vs Serial NoOverlap runs must match recall, probed count, SafeOut/SafeIn/Uncertain counts, candidate budget selection, rerank count, vector read count and payload read count within documented tolerance.
  - Any performance delta can only be attributed to scheduling if the above counts match.
