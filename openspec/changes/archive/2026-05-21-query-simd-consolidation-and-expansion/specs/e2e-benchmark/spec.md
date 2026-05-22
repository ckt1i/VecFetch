## ADDED Requirements

### Requirement: E2E benchmark SHALL expose query SIMD capability toggles in exported results
The benchmark workflow SHALL make query-path SIMD capability toggles explicit in exported results so that SIMD-on, fallback, and mixed configurations can be compared under the same operating point. At minimum, exported metadata MUST be able to distinguish address-decode SIMD, rerank batched-distance SIMD, coarse-select SIMD, Stage2 block-first, and Stage2 batch-classify states when those capabilities are present.

When a newly introduced SIMD runtime path is split into Phase 1 and Phase 2, exported metadata MUST be able to distinguish whether the run used:

- fallback/reference
- Phase 1 conservative implementation
- Phase 2 stronger specialization

#### Scenario: SIMD toggle state is exported
- **WHEN** a benchmark run enables or disables a query-path SIMD capability
- **THEN** the exported result MUST include the corresponding capability state
- **AND** the result MUST remain identifiable as SIMD-on or fallback for later comparison

#### Scenario: Phase 1 and Phase 2 runtime paths are benchmarked
- **WHEN** a benchmark run compares a newly introduced SIMD runtime path across rollout phases
- **THEN** the exported result MUST indicate whether the run used fallback, Phase 1, or Phase 2
- **AND** that distinction MUST be sufficient to compare correctness and performance across phases

### Requirement: Benchmark validation SHALL support both query-only and full E2E SIMD comparison
The benchmark workflow SHALL support validating query-path SIMD capabilities under both `query_only` and `full_e2e` modes. Query-only runs MUST preserve stage breakdown observability, and full E2E runs MUST preserve recall comparison.

#### Scenario: Query-only SIMD validation is run
- **WHEN** a query-path SIMD capability is validated under `query_only`
- **THEN** the result MUST export the relevant stage breakdown fields needed to attribute the SIMD change
- **AND** those fields MUST remain comparable with the fallback path

#### Scenario: Full E2E SIMD validation is run
- **WHEN** a query-path SIMD capability is validated under `full_e2e`
- **THEN** the result MUST report recall and latency under the same operating point as the fallback path
- **AND** the result MUST be sufficient to determine whether semantics changed

#### Scenario: Phase 2 SIMD validation is run
- **WHEN** a newly introduced SIMD runtime path is validated in its Phase 2 form
- **THEN** the benchmark MUST still be able to compare it directly against both fallback and Phase 1 under the same operating point
- **AND** the result MUST be sufficient to determine whether Phase 2 provided benefit beyond Phase 1 without changing semantics

## MODIFIED Requirements

### Requirement: E2E benchmark 必须报告 Stage 1 和 Stage 2 的候选流统计
用于 epsilon 验证和 resident 主路径分析的 benchmark 输出 MUST 包含分析候选流与 query 主路径变化所需的统计，而不能只输出最终延迟。除现有 Stage 1 / Stage 2 候选计数外，resident 主路径运行还 MUST 导出 `coarse_select_ms`、`probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms`、`probe_classify_ms` 和 `probe_submit_ms`。

当 query-path SIMD 能力在细粒度诊断模式下被验证时，benchmark 输出还 MUST 保持 `probe_stage2_collect_ms`、`probe_stage2_kernel_ms`、`probe_stage2_scatter_ms` 等子项可见，并确保 current path 与 fallback path 在 `query_only` 与 `full_e2e` 两种模式下都能同口径比较。

#### Scenario: 候选流统计被导出
- **当** 一次 epsilon 验证运行结束时
- **则** 输出中必须包含 `avg_total_probed`
- **并且** 输出中必须包含 Stage 1 `SafeOut`
- **并且** 输出中必须包含 Stage 2 `SafeOut`
- **并且** 输出中必须包含 Stage 2 `Uncertain`
- **并且** 输出中必须包含 `avg_probe_time_ms`

#### Scenario: Resident query-path breakdown is exported
- **WHEN** benchmark 在 resident 优化路径上运行
- **THEN** 输出中 MUST 包含 `avg_coarse_select_ms`
- **AND** MUST 包含 `avg_probe_prepare_ms`
- **AND** MUST 包含 `avg_probe_stage1_ms`
- **AND** MUST 包含 `avg_probe_stage2_ms`
- **AND** MUST 包含 `avg_probe_classify_ms`
- **AND** MUST 包含 `avg_probe_submit_ms`

#### Scenario: Fine-grained Stage2 comparison is preserved
- **WHEN** benchmark 以细粒度诊断模式验证 query-path SIMD
- **THEN** 输出中 MUST 包含 `avg_probe_stage2_collect_ms`
- **AND** MUST 包含 `avg_probe_stage2_kernel_ms`
- **AND** MUST 包含 `avg_probe_stage2_scatter_ms`
- **AND** 这些字段 MUST 能在 current path 与 fallback path 之间直接比较
