## ADDED Requirements

### Requirement: Mask-aware Stage2 kernel
The system SHALL provide a Stage2 ExRaBitQ compact kernel path that accepts a block-local lane mask and computes output only for lanes selected by the mask, while preserving the full-lane kernel as a fallback.

#### Scenario: Sparse lane mask
- **WHEN** Stage2 receives a compact v11 parallel block with only a subset of valid lanes marked uncertain
- **THEN** the mask-aware kernel computes IP values for the marked lanes and does not require computing unmarked lanes

#### Scenario: Full lane mask
- **WHEN** the lane mask covers every valid lane in the block
- **THEN** the system may use either the mask-aware kernel or the existing full-lane kernel and MUST produce equivalent lane outputs for all valid lanes

#### Scenario: Unsupported layout fallback
- **WHEN** compact v11 parallel block data is unavailable or the storage layout is unsupported by the mask-aware kernel
- **THEN** the system MUST fall back to the existing Stage2 kernel path without changing query results

### Requirement: Query correctness preservation
The system SHALL preserve query semantics, recall, top-k ordering, CRC behavior, and rerank input behavior when the mask-aware Stage2 kernel is enabled.

#### Scenario: End-to-end recall validation
- **WHEN** benchmark runs with real ground truth, `--skip-gt 0`, and the same search parameters before and after enabling the mask-aware kernel
- **THEN** recall@1, recall@5, recall@10, result count, and top-k ordering MUST match the existing Stage2 path within the established floating-point tolerance

#### Scenario: Kernel equivalence validation
- **WHEN** a test compares mask-aware outputs against the full-lane kernel for selected lanes across randomized lane masks
- **THEN** every selected lane output MUST match the full-lane output within the existing Stage2 kernel numeric tolerance

### Requirement: Stage2 lane utilization observability
The system SHALL expose enough Stage2 lane utilization statistics to explain whether the mask-aware kernel reduced unnecessary lane computation.

#### Scenario: Benchmark statistics
- **WHEN** `bench_e2e` writes pipeline statistics after a query round
- **THEN** the JSON output MUST include Stage2 masked-kernel call count, requested lane count, skipped lane count, and total valid lane count while preserving all existing JSON fields

#### Scenario: Disabled or fallback path
- **WHEN** the mask-aware kernel is not used for a query
- **THEN** the Stage2 lane utilization statistics MUST remain well-defined and MUST NOT break existing benchmark output consumers

### Requirement: No parameter-driven performance claim
The system SHALL evaluate this change without relying on epsilon, nprobe, early-stop, or two-level routing parameter changes as the source of performance improvement.

#### Scenario: Official validation run
- **WHEN** performance validation is reported for this change
- **THEN** the before/after commands MUST use the same search parameters, real GT, `--skip-gt 0`, and `--early-stop 0`, with only the mask-aware kernel implementation differing
