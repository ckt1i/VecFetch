## MODIFIED Requirements

### Requirement: E2E benchmark MUST preserve prepare and Stage1 fine-grained observability for fused-kernel comparison
在 query 主路径继续推进 fused `quantize + lut_build` 和 Stage1 后续 SIMD 优化时，benchmark 输出 SHALL 保持 prepare 与 Stage1 的细粒度子项可见，以便同口径比较参考路径、保留两段版和融合版。对于 padded-Hadamard 评估，benchmark 输出还 MUST 显式报告 `logical_dim`、`effective_dim`、`padding_mode`、`rotation_mode`，并能区分 prepare 中的 rotation 成本与 quantize/LUT 成本。

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

#### Scenario: Padded-Hadamard metadata is exported with query results
- **WHEN** benchmark 在 padded-Hadamard 候选路径上运行
- **THEN** 输出 MUST 包含 `logical_dim`、`effective_dim`、`padding_mode` 和 `rotation_mode`
- **AND** 输出 MUST 足以区分 baseline random rotation 与 padded Hadamard

### Requirement: E2E benchmark 必须输出新分段统计字段
在线 E2E benchmark MUST 输出至少以下字段：`probe_ms`、`prefetch_submit_ms`、`prefetch_wait_ms`、`safein_payload_prefetch_ms`、`candidate_collect_ms`、`pool_vector_read_ms`、`rerank_compute_ms`、`remaining_payload_fetch_ms`、`e2e_ms`、`num_candidates_buffered`、`num_candidates_reranked`、`num_safein_payload_prefetched`、`num_remaining_payload_fetches`。对于 padded-Hadamard 比较，输出还 MUST 支持记录 prepare-rotation 与 prepare-quant-lut 两个总览字段。

#### Scenario: benchmark 输出新统计字段
- **WHEN** benchmark 完成一次查询轮次并写出结果
- **THEN** 结果中必须包含约定的新分段时间和候选计数字段

#### Scenario: Padded-Hadamard comparison exposes rotation tradeoff
- **WHEN** benchmark 导出一条 padded-Hadamard 比较结果
- **THEN** 结果 MUST 至少包含 `prepare_rotation_ms` 或等价字段
- **AND** MUST 包含 `prepare_quant_lut_ms` 或等价字段
- **AND** MUST 让使用者判断“更快的 rotation”是否被“更大的 effective_dim 线性成本”抵消
