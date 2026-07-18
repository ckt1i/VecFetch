## ADDED Requirements

### Requirement: Benchmark cleanup validation SHALL compare against pre-cleanup best results
After redundant feature removal, benchmark validation SHALL compare the cleaned implementation against the best pre-cleanup result available under the same dataset, index, and frozen query parameters. The validation MUST report recall, average latency or QPS, total probed count, and rerank or candidate count when available.

#### Scenario: Same-index warm validation is executed
- **WHEN** cleanup implementation is complete
- **THEN** validation MUST reuse existing indexes rather than rebuilding unless the index is unavailable or incompatible
- **AND** the benchmark record MUST include enough metadata to identify dataset, index path, active ex-bits, resident ex-bits, nprobe, topk, and two-level routing settings

#### Scenario: Cleanup result is compared with tolerance
- **WHEN** post-cleanup metrics are compared with pre-cleanup best metrics
- **THEN** `recall@10` absolute loss MUST NOT exceed `0.002`
- **AND** average latency MUST NOT regress by more than `3%` unless repeated runs show the difference is within noise
- **AND** QPS MUST NOT drop by more than `3%` unless repeated runs show the difference is within noise

### Requirement: Benchmark output SHALL remove abandoned feature fields
Benchmark output SHALL stop treating removed feature-specific fields as formal output schema. It MUST preserve official fields needed for frozen-path comparison and mechanism attribution.

#### Scenario: Removed feature fields are absent
- **WHEN** benchmark JSON or CSV output is generated after cleanup
- **THEN** fields dedicated only to Stage2 progressive pruning, Stage1 envelope skip, address sorting, and budgeted early submit MUST NOT be required
- **AND** parsers used by cleanup validation MUST not fail because those removed fields are absent

#### Scenario: Frozen-path fields are preserved
- **WHEN** benchmark output is generated after cleanup
- **THEN** it MUST still include recall, latency or QPS, preload memory when applicable, coarse select time, probe prepare time, Stage1 time, Stage2 time, submit time, total probed count, and rerank or candidate count when applicable
