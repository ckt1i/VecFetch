## ADDED Requirements

### Requirement: Formal baseline E2E benchmark SHALL use coupled timing semantics
The `e2e-benchmark` capability SHALL support the formal baseline study's coupled `search + payload` timing contract.

#### Scenario: E2E timing starts at query submission
- **WHEN** the benchmark is run in formal baseline study mode
- **THEN** it SHALL start timing when the query vector is submitted into the system
- **AND** the measured interval SHALL include all retrieval work needed before payload fetch begins

#### Scenario: E2E timing ends at payload completion
- **WHEN** the benchmark determines the final records that must be fetched for a query
- **THEN** it SHALL trigger payload fetch within the same query path
- **AND** the E2E timer SHALL stop only after the required payload read completes

### Requirement: Formal study runs SHALL export a stable coupled-E2E metric schema
The `e2e-benchmark` capability SHALL emit a metric schema that is sufficient for the formal baseline study's comparison, aggregation, and tracking steps.

#### Scenario: Mandatory coupled-E2E fields are exported
- **WHEN** a formal-study E2E run completes
- **THEN** the benchmark output SHALL include `ann_core_ms`, `payload_fetch_ms`, `bytes_read`, `fetch_count`, `e2e_ms`, a top-k-matched recall metric for the configured `topk`, and percentile latency fields including `p99_ms`
- **AND** the parameter identity in the output SHALL be sufficient to reconstruct the originating system configuration

#### Scenario: Formal-study output location is stable
- **WHEN** a formal-study E2E run writes its per-run results
- **THEN** it SHALL write under `/home/zcq/VDB/baselines/formal-study/outputs/e2e/{dataset}/{system}/{run_id}/`
- **AND** the benchmark SHALL support generation of a dataset-level summary file for downstream aggregation

### Requirement: Formal-study E2E mode SHALL remain distinct from payload-only diagnostics
The `e2e-benchmark` capability SHALL preserve the distinction between primary coupled E2E results and payload microbench diagnostics.

#### Scenario: Payload-only measurements do not override the formal E2E result
- **WHEN** payload backend measurements are collected separately
- **THEN** they SHALL be exported to the payload microbench output family
- **AND** the benchmark SHALL NOT present those diagnostic measurements as the primary formal-study E2E result
