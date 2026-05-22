## ADDED Requirements

### Requirement: E2E benchmark SHALL formalize the hot-path follow-up experiment protocol
在线 E2E benchmark MUST 为本轮 `submit-batch` 与 Stage2 follow-up 提供稳定实验协议。该协议 MUST 支持在固定 operating point 下执行多次重复实验，并允许在同一批参数上导出 full E2E 与 query-only perf 结果，以区分真实收益与噪声。

#### Scenario: Follow-up experiment repeats are comparable
- **WHEN** 某个 `submit-batch` 或 Stage2 follow-up 变体被评估
- **THEN** benchmark 协议 MUST 允许在固定 operating point 下重复执行多次
- **AND** 输出 MUST 足以比较均值与离散度，而不是只保留单次结果

#### Scenario: Query-only and E2E results are both available
- **WHEN** follow-up 优化项进入正式评估
- **THEN** 同一 operating point MUST 同时可获得 query-only perf 结果与 full E2E 结果
- **AND** 两者 MUST 使用相同的查询参数和结果口径

### Requirement: E2E benchmark SHALL expose adaptive submit attribution
针对三层 flush `submit-batch`，benchmark 输出 MUST 足以解释 flush 机制本身是否带来收益，而不仅是最终总时间变化。输出 MUST 至少能够区分 flush 次数、flush 规模和 `probe_submit` 相关 CPU 成本。

#### Scenario: Adaptive submit fields are exported
- **WHEN** benchmark 在三层 flush 的 resident 路径上运行
- **THEN** 结果 MUST 包含 flush 次数或等价统计
- **AND** MUST 包含每次 flush 请求规模的聚合信息
- **AND** `probe_submit_ms` 仍然 MUST 作为总提交 CPU 成本单独可见

### Requirement: E2E benchmark SHALL preserve Stage2 follow-up attribution boundaries
针对 Stage2 `collect/scatter` follow-up，benchmark 输出 MUST 继续支持区分 Stage2 内部成本边界。若某轮运行开启细粒度归因，结果 MUST 足以区分 collect、kernel 与 scatter 的变化；若运行于低开销口径，结果 MUST 至少保留总 Stage2 字段并显式说明口径限制。

#### Scenario: Fine-grained Stage2 fields remain comparable when enabled
- **WHEN** benchmark 以细粒度 Stage2 归因模式运行
- **THEN** 输出 MUST 能够区分 Stage2 collect、kernel 与 scatter 的时间字段
- **AND** 这些字段 MUST 可用于比较 follow-up 优化前后的变化

#### Scenario: Low-overhead mode still declares attribution limits
- **WHEN** benchmark 运行于低开销统计模式
- **THEN** 输出 MUST 至少保留总 Stage2 时间字段
- **AND** MUST 让分析结果能够区分“字段为零”是因为未计量还是因为成本确实消失
