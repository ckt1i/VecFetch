## ADDED Requirements

### Requirement: Fused Stage1 evaluation
The system SHALL provide a FastScan Stage1 fused evaluation path that computes dequantized distances, SafeOut mask, and optional SafeIn mask from one `raw_accu` block without requiring separate mask passes over `dists`.

The fused path SHALL use the current interval-bound classification semantics:

- SafeOut threshold is `safeout_frontier_upper + safeout_margin_factor * block_norm`.
- SafeIn threshold is `safein_d_k - safein_margin_factor * block_norm`.
- Legacy `dynamic_d_k + 2 * margin` and `safein_d_k - 2 * margin` formulas MUST NOT be used.

#### Scenario: SafeOut mask equivalence
- **WHEN** fused Stage1 evaluation receives the same `raw_accu`, block norms, query constants, `safeout_frontier_upper`, and SafeOut margin factor as the legacy `FastScanDequantize` plus current `FastScanSafeOutMask` path
- **THEN** the fused path MUST produce identical dequantized distances and the same SafeOut mask for all valid lanes

#### Scenario: SafeIn enabled
- **WHEN** fused Stage1 evaluation is called with SafeIn enabled and the same `safein_d_k` / SafeIn margin factor as the current `FastScanSafeInMask` path
- **THEN** the fused path MUST produce the same SafeIn mask as the legacy `FastScanSafeInMask` path for all valid lanes

#### Scenario: SafeIn disabled
- **WHEN** fused Stage1 evaluation is called with SafeIn disabled
- **THEN** the fused path MUST return `safein_mask=0` and MUST NOT change SafeOut or distance results

#### Scenario: Heap-not-full frontier
- **WHEN** fused Stage1 evaluation receives `safeout_frontier_upper=+inf`
- **THEN** the fused path MUST produce no SafeOut lanes, matching the current heap-not-full behavior

### Requirement: ClusterProber Stage1 integration
The system SHALL use the fused Stage1 evaluation path in `ClusterProber::Probe` while preserving Stage2, candidate emission, CRC, and statistics semantics.

#### Scenario: Candidate classification equivalence
- **WHEN** `ClusterProber::Probe` processes the same parsed cluster and prepared query before and after fused Stage1 integration
- **THEN** Stage1 SafeOut/SafeIn/Uncertain counts and emitted candidate batches MUST match the legacy path

#### Scenario: Error-bound plumbing preservation
- **WHEN** a lane survives fused Stage1 and is emitted to candidate batching or CRC estimate buffering
- **THEN** the emitted `est_error` / error-bound value MUST match the current non-fused path

#### Scenario: Stage2 handoff preservation
- **WHEN** a lane remains uncertain after fused Stage1 evaluation
- **THEN** the Stage2 handoff MUST receive the same `est_dist_s1`, global index, margins, and block norm as the legacy path

### Requirement: Observability compatibility
The system SHALL preserve existing benchmark and per-query JSON fields while allowing additional fused Stage1 diagnostic fields.

#### Scenario: Existing fields preserved
- **WHEN** `bench_e2e` writes results after fused Stage1 integration
- **THEN** all pre-existing `pipeline_stats` and per-query sample field names MUST remain present

#### Scenario: Fused path diagnostics
- **WHEN** fused Stage1 diagnostics are emitted
- **THEN** they MUST be additive and MUST NOT redefine the meaning of existing Stage1 timing fields

### Requirement: No parameter-driven performance claim
The system SHALL evaluate this change without relying on epsilon, nprobe, early-stop, or routing parameter changes as the source of performance improvement.

#### Scenario: Official validation run
- **WHEN** performance validation is reported for this change
- **THEN** the before/after commands MUST use the same search parameters, real GT, `--skip-gt 0`, and `--early-stop 0`, with only the fused Stage1 implementation differing
