## MODIFIED Requirements

### Requirement: E2E benchmark MUST preserve prepare and Stage1 fine-grained observability for fused-kernel comparison
在 query 主路径继续推进 fused `quantize + lut_build` 和 Stage1 后续 SIMD 优化时，benchmark 输出 SHALL 保持 prepare 与 Stage1 的细粒度子项可见，以便同口径比较参考路径、保留两段版和融合版。对于 padded-Hadamard 评估，benchmark 输出还 MUST 显式报告 `logical_dim`、`effective_dim`、`padding_mode`、`rotation_mode`，并能区分 prepare 中的 rotation 成本与 quantize/LUT 成本。对于 raw-storage 回退到 `logical_dim` 的 padded-Hadamard 方案，benchmark 输出还 MUST 让使用者区分 raw vector plane footprint 与 compressed code plane footprint。

prepare 至少 MUST 包含：

- `probe_prepare_subtract_ms`
- `probe_prepare_normalize_ms`
- `probe_prepare_quantize_ms`
- `probe_prepare_lut_build_ms`

Stage1 至少 MUST 包含：

- `probe_stage1_estimate_ms`
- `probe_stage1_mask_ms`
- `probe_stage1_iterate_ms`
- `probe_stage1_classify_only_ms`
- `probe_submit_ms`

#### Scenario: Fused prepare is benchmarked under the same field schema
- **WHEN** 系统切换到 fused `quantize + lut_build` 路径
- **THEN** benchmark 输出 MUST 仍然保留 prepare 四段字段
- **AND** 这些字段 MUST 能与参考路径直接对比

#### Scenario: Padded-Hadamard metadata is exported with storage split visibility
- **WHEN** benchmark 在 padded-Hadamard 候选路径上运行且 raw vector plane 使用 `logical_dim`
- **THEN** 输出 MUST 包含 `logical_dim`、`effective_dim`、`padding_mode` 和 `rotation_mode`
- **AND** 输出 MUST 足以区分 raw vector plane 与 compressed code plane 的 footprint

### Requirement: E2E benchmark 必须报告 Stage 1 和 Stage 2 的候选流统计
用于 epsilon 验证和 resident 主路径分析的 benchmark 输出 MUST 包含分析候选流与 query 主路径变化所需的统计，而不能只输出最终延迟。除现有 Stage 1 / Stage 2 候选计数外，resident 主路径运行还 MUST 导出 `coarse_select_ms`、`probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms`、`probe_classify_ms` 和 `probe_submit_ms`。

#### Scenario: 候选流统计被导出
- **WHEN** 一次 epsilon 验证运行结束时
- **THEN** 输出中必须包含 `avg_total_probed`
- **AND** 输出中必须包含 Stage 1 `SafeOut`
- **AND** 输出中必须包含 Stage 2 `SafeOut`
- **AND** 输出中必须包含 Stage 2 `Uncertain`
- **AND** 输出中必须包含 `avg_probe_time_ms`

#### Scenario: Resident query-path breakdown is exported
- **WHEN** benchmark 在 resident 优化路径上运行
- **THEN** 输出中 MUST 包含 `avg_coarse_select_ms`
- **AND** MUST 包含 `avg_probe_prepare_ms`
- **AND** MUST 包含 `avg_probe_stage1_ms`
- **AND** MUST 包含 `avg_probe_stage2_ms`
- **AND** MUST 包含 `avg_probe_classify_ms`
- **AND** MUST 包含 `avg_probe_submit_ms`
