## ADDED Requirements

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
