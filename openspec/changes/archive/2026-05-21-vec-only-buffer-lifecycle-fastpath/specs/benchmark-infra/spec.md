## ADDED Requirements

### Requirement: CPU hot-path optimization benchmark SHALL report real recall
用于 CPU hot-path 优化结论的 benchmark SHALL 使用真实 ground truth，使输出中的 recall 字段可用。`query-only + skip-gt` 运行 MAY 用作热点定位或调试，但 MUST NOT 作为最终 recall 或正式性能结论的唯一依据。

#### Scenario: Optimization benchmark uses available ground truth
- **WHEN** 运行 MSMARCO `fht_kac_rotator` resident/full-preload 优化验证
- **THEN** benchmark SHALL 使用 external GT 或 computed GT
- **AND** 输出 SHALL 满足 `recall_available=true`
- **AND** 输出 SHALL 包含 `recall@1`、`recall@5` 和 `recall@10`

#### Scenario: Query-only profile is marked as diagnostic only
- **WHEN** benchmark 以 `query-only + skip-gt` 或等价方式运行
- **THEN** 该结果 SHALL 被视为 diagnostic-only
- **AND** 报告中 MUST NOT 将该结果的 zero recall 字段解释为算法 recall

### Requirement: Fixed vector buffer sweep SHALL expose lifecycle diagnostics
benchmark 基础设施 SHALL 支持对 fixed vector buffer count 进行 sweep，并导出足够的结构化指标来判断 vec-only buffer lifecycle fast path 的收益。

#### Scenario: Benchmark config records fixed vector buffer count
- **WHEN** benchmark 使用显式 fixed vector buffer count
- **THEN** 输出配置 SHALL 记录该 count
- **AND** 结果 SHALL 仍记录 `io_queue_depth`，以便区分 queue capacity 与 registered buffer capacity

#### Scenario: Lifecycle diagnostics are emitted for vec-only optimization
- **WHEN** resident vector-only buffer lifecycle fast path 被用于 query benchmark
- **THEN** 输出 SHALL 包含 fixed-buffer hit/miss、vector-only read request 数、payload/all read request 数、`probe_submit_ms` 和 `probe_submit_vec_only_emit_ms`
- **AND** 这些字段 SHALL 能够比较不同 fixed vector buffer count 下的 submit-path 行为

#### Scenario: Acceptance comparison keeps recall stable
- **WHEN** 比较优化前后或不同 fixed vector buffer count 的 benchmark 结果
- **THEN** recall 指标 SHALL 在同一 query 集和同一 ground truth 下比较
- **AND** 性能收益 MUST NOT 以 recall 回退为代价
