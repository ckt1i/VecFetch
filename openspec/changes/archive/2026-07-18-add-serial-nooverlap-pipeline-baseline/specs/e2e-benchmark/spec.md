## ADDED Requirements

### Requirement: Online benchmark SHALL expose execution-mode selection
The online benchmark SHALL allow selecting the query execution mode from the command line. The default MUST remain the current overlap pipeline. A `serial-no-overlap` option MUST select the fully staged serial pipeline used for strong No Pipeline ablation.

#### Scenario: Default benchmark remains overlap mode
- **WHEN** the benchmark is run without an execution-mode option
- **THEN** it MUST use the existing overlap pipeline behavior
- **AND** existing scripts that do not request serial no-overlap MUST remain compatible

#### Scenario: Serial no-overlap mode is selectable
- **WHEN** the benchmark is run with `--execution-mode serial-no-overlap`
- **THEN** query execution MUST use the serial no-overlap query pipeline
- **AND** the output metadata MUST record `execution_mode=serial_no_overlap`

### Requirement: Benchmark output SHALL distinguish serial no-overlap from serial drain diagnostics
Benchmark output SHALL distinguish the strong Serial NoOverlap baseline from weaker diagnostics such as `serial_data_drains`. Results produced by `--serial-data-drain 1` alone MUST NOT be labeled as the strong No Pipeline baseline.

#### Scenario: Serial drain remains separately identified
- **WHEN** a run uses `--serial-data-drain 1` with default overlap execution mode
- **THEN** the output MUST record `serial_data_drains=true`
- **AND** the output MUST NOT record `execution_mode=serial_no_overlap`

#### Scenario: Strong No Pipeline is explicit
- **WHEN** a run is intended as the strong No Pipeline baseline
- **THEN** it MUST record `execution_mode=serial_no_overlap`
- **AND** it MUST record that async candidate-data overlap is disabled

### Requirement: Benchmark output SHALL report serial read and attribution fields
Benchmark output for serial no-overlap runs SHALL include fields that make the lack of overlap and the cost of serialized access visible. These fields MUST be sufficient to compare Full Pipeline and Serial NoOverlap under the same candidate and read counts.

#### Scenario: Serial timing fields are exported
- **WHEN** a serial no-overlap benchmark completes
- **THEN** the output MUST include average serial raw-vector read time
- **AND** it MUST include average serial full-record read time
- **AND** it MUST include average serial final payload read time
- **AND** it MUST include total query time and tail latency under the same schema as overlap runs

#### Scenario: Serial count fields are exported
- **WHEN** a serial no-overlap benchmark completes
- **THEN** the output MUST include raw-vector read request count and bytes
- **AND** it MUST include full-record read request count and bytes
- **AND** it MUST include final payload read request count and bytes
- **AND** it MUST include exact rerank candidate count

### Requirement: Pipeline ablation results SHALL be valid only under matched semantics
The pipeline ablation reporting workflow SHALL mark a Full Pipeline vs Serial NoOverlap pair as valid only when recall and mechanism counts match within the documented tolerance. Pairs with mismatched candidate generation or read semantics MUST be excluded from pipeline speedup claims.

#### Scenario: Matched pair is accepted
- **WHEN** Full Pipeline and Serial NoOverlap are run on the same dataset, index, queries, top-k, nprobe, active bits, resident bits, SafeOut epsilon, two-level routing and candidate budget
- **THEN** the summary workflow MAY report their latency/QPS delta as pipeline effect
- **AND** it MUST include recall, total probed, SafeOut/SafeIn/Uncertain, rerank count and read-count deltas

#### Scenario: Mismatched pair is rejected
- **WHEN** a Full Pipeline vs Serial NoOverlap pair has unexplained recall, candidate count, rerank count or read-count mismatch beyond tolerance
- **THEN** the summary workflow MUST mark the pair invalid for pipeline attribution
- **AND** it MUST not use that pair to support an independent pipeline contribution claim
