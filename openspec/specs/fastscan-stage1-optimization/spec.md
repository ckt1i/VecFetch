## ADDED Requirements

### Requirement: FastScan Stage1 SHALL minimize online query preparation state
单 bit FastScan 的 query prepare 路径 MUST 将 query 无关或可复用的结构前移，并将在线查询需要长期保留的 `PreparedQuery` 字段压缩到最小必要集合。实现 MAY 使用局部 scratch buffer 作为中间过渡，但这些中间态 MUST 不成为 query 路径长期暴露的重量级状态。

#### Scenario: Temporary FastScan preparation state does not escape query preparation
- **WHEN** 一次 resident query 完成 Stage1 准备
- **THEN** 仅用于构建 LUT 或中间量化的临时 buffer MUST 保持为局部 scratch 状态
- **AND** 它 MUST 不作为下游 probe 路径的长期依赖字段泄露出去

#### Scenario: Stage1 equivalence is preserved after preparation-state slimming
- **WHEN** 优化后的 prepare 路径处理同一个 query 与 centroid
- **THEN** 生成的 Stage1 distance estimate MUST 与参考实现保持等价
- **AND** 后续 Stage2 / rerank 结果语义 MUST 保持不变

### Requirement: FastScan Stage1 SHALL expose and optimize prepare and classification substeps
系统 MUST 把单 bit FastScan 的主路径至少区分为 `probe_prepare_ms`、`probe_stage1_ms` 和 `probe_stage2_ms` 三段，并 SHALL 允许针对 `PrepareQueryRotatedInto`、LUT 构建和 `EstimateDistanceFastScan` 主循环分别实施优化。优化后的实现 MUST 保留这些观测字段，以便 benchmark 和 perf 能归因收益来源。

#### Scenario: Benchmark output keeps Stage1 observability after optimization
- **WHEN** benchmark 在优化后的 resident 路径上运行
- **THEN** 输出 MUST 继续包含 `probe_prepare_ms`
- **AND** 输出 MUST 继续包含 `probe_stage1_ms`
- **AND** 输出 MUST 继续包含 `probe_stage2_ms`

#### Scenario: Query path optimization does not collapse Stage1 into an opaque total
- **WHEN** 后续实现压缩 `PrepareQueryRotatedInto`、LUT 构建或 `EstimateDistanceFastScan`
- **THEN** 这些优化 MUST 仍然能够被映射回现有 Stage1 相关时间字段
- **AND** 系统 MUST 不以重新合并统计字段的方式隐藏收益或回归

## ADDED Requirements (from fastscan-prepare-hotpath-optimization)

### Requirement: FastScan Stage1 prepare SHALL keep fine-grained prepare observability
在优化后的 resident FastScan Stage1 路径中，系统 MUST 继续独立观测 `prepare_subtract`、`prepare_normalize`、`prepare_quantize` 和 `prepare_lut_build`。后续实现 MAY 重构 fused prepare 内部结构，但 MUST 不通过合并、移除或黑盒化这些边界来隐藏收益或回归。

#### Scenario: Prepare substeps remain visible after hot-path optimization
- **WHEN** benchmark 或分析模式在优化后的 FastScan Stage1 prepare 路径上运行
- **THEN** 输出 MUST 继续包含 `prepare_subtract`
- **AND** MUST 继续包含 `prepare_normalize`、`prepare_quantize` 和 `prepare_lut_build`

### Requirement: FastScan Stage1 optimization SHALL prioritize estimate kernel over post-estimate control flow
在 resident 主工作点下，若系统继续优化 Stage1 路径，优化边界 MUST 允许将重点放在 `EstimateDistanceFastScan` 主估计循环，而不是重新把 `mask`、`iterate` 和 `classify` 后处理与估计主循环混合为不可区分的一体路径。

#### Scenario: Estimate kernel remains a distinct optimization boundary
- **WHEN** 系统对 FastScan Stage1 进行后续优化
- **THEN** `EstimateDistanceFastScan` 主估计循环 MUST 保持为独立可定位的优化边界
- **AND** `mask`、`iterate` 和 `classify` 后处理 MUST 不重新吞并该边界

## ADDED Requirements (from accumulate-block-chunked-template)

### Requirement: FastScan AccumulateBlock SHALL provide dimension-multiple chunked fast paths
`simd::AccumulateBlock` SHALL provide internal fast paths for dimensions that are multiples of 64 or 32 without changing its public API, input layout, output layout, or observable Stage1 distance semantics. For dimensions divisible by 64, the system MUST prefer the 64-dim chunk path. For dimensions divisible by 32 but not 64, the system MUST use the 32-dim chunk path. For other legal dimensions, the system MUST fall back to the generic implementation.

#### Scenario: n*64 dimension uses 64-dim chunk path
- **WHEN** `simd::AccumulateBlock` is called with a legal dimension divisible by 64
- **THEN** the implementation MUST route through the 64-dim chunked accumulation path
- **AND** the produced `result[32]` MUST match the generic/reference accumulation result

#### Scenario: n*32 non-n*64 dimension uses 32-dim chunk path
- **WHEN** `simd::AccumulateBlock` is called with a legal dimension divisible by 32 but not divisible by 64
- **THEN** the implementation MUST route through the 32-dim chunked accumulation path
- **AND** the produced `result[32]` MUST match the generic/reference accumulation result

#### Scenario: Unsupported chunk dimension falls back
- **WHEN** `simd::AccumulateBlock` is called with a legal dimension that is not divisible by 32
- **THEN** the implementation MUST use the generic fallback path
- **AND** the produced Stage1 estimate MUST preserve pre-change behavior

### Requirement: AccumulateBlock chunk specialization SHALL preserve FastScan layout contracts
The chunked `AccumulateBlock` implementation SHALL preserve the existing packed code layout, packed LUT layout, lane accumulation order, and final `result[32]` reduction semantics. The optimization MUST NOT require changes to query prepare, LUT build, packed code storage, `EstimateDistanceFastScan`, or `EvaluateStage1FastScan` caller contracts.

#### Scenario: Existing callers remain unchanged
- **WHEN** `EstimateDistanceFastScan` or `EvaluateStage1FastScan` calls `simd::AccumulateBlock`
- **THEN** the caller MUST NOT need to provide a new dimension-specific API or additional dispatch metadata
- **AND** Stage1 distance estimates MUST remain equivalent to the reference path

#### Scenario: Packed LUT bytes remain compatible
- **WHEN** query prepare emits packed LUT bytes using the existing layout
- **THEN** the chunked `AccumulateBlock` path MUST consume those bytes without requiring a different LUT packing format
- **AND** fused or non-fused LUT build paths MUST remain compatible

### Requirement: AccumulateBlock optimization SHALL be validated across representative n*32 and n*64 dimensions
The system SHALL include correctness coverage for representative 32-multiple and 64-multiple dimensions, including but not limited to MSMARCO-style 768 dimensions and non-64 32-multiple dimensions. Validation MUST compare chunked accumulation against a reference implementation and MUST preserve end-to-end recall/top-k behavior in benchmark validation.

#### Scenario: Representative dimensions are tested
- **WHEN** SIMD FastScan tests run
- **THEN** they MUST cover at least one `n*64` dimension larger than 512
- **AND** they MUST cover at least one `n*32` dimension that is not divisible by 64
- **AND** chunked accumulation results MUST match reference accumulation results

#### Scenario: End-to-end benchmark preserves recall
- **WHEN** the optimized code is benchmarked with real ground truth
- **THEN** recall and top-k semantics MUST remain unchanged relative to the pre-change implementation
- **AND** any reported speedup MUST be based on runs with real recall enabled rather than `skip-gt` only
