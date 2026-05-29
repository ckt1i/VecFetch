## ADDED Requirements

### Requirement: Benchmark output SHALL expose probe-submit fast-path diagnostics
benchmark 输出 SHALL 提供 probe-submit fast path 的结构化诊断信息，以便将回归和收益归因到 request planning、slot allocation、SQE preparation、fixed-buffer availability 以及 rerank 向量存储上。

#### Scenario: Submit-path diagnostics are emitted
- **WHEN** 某次 query benchmark 完成，并且 submit-path 诊断可用或被启用
- **THEN** 输出 SHALL 包含 vector-only 请求数、all-read 请求数、payload 请求数、fixed-buffer 命中数、fixed-buffer 未命中数，以及 vector-only emit 计时
- **AND** 现有聚合字段如 `probe_submit_ms`、`probe_submit_prepare_vec_only_ms` 和 `uring_submit_ms` SHALL 保持可用

#### Scenario: Rerank vector storage diagnostics are emitted
- **WHEN** query benchmark 使用 buffered batch rerank
- **THEN** 输出 SHALL 包含 rerank 向量分配或 slab 计时、向量拷贝计时，以及 buffered candidate 数量
- **AND** 这些字段 SHALL 能够将分配开销与距离计算开销区分开来

### Requirement: Probe-submit optimization SHALL have benchmark acceptance coverage
实现 SHALL 包含可重复的 benchmark 覆盖，用于 MSMARCO `fht_kac_rotator` 的 query-only resident/full-preload 模式，并使用结构化指标对比优化前后的 submit 行为。

#### Scenario: MSMARCO resident query-only validation compares key metrics
- **WHEN** 优化后的路径与现有 `fht_kac_rotator` 索引进行对比
- **THEN** benchmark SHALL 报告 `avg_query_time_ms`、`probe_submit_ms`、`probe_submit_prepare_vec_only_ms`、`uring_submit_ms`、`io_wait_ms`、fixed-buffer 命中/未命中计数，以及 candidate 数量
- **AND** 如果有 ground truth 或确定性的 top-k baseline，可检查结果语义 MUST 与现有路径一致
