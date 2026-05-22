## ADDED Requirements

### Requirement: The experiment workflow SHALL compare baseline random rotation against padded Hadamard with the same serving operating point
The system SHALL define an experiment workflow that compares:

- baseline `logical_dim` random rotation
- padded `effective_dim` Hadamard rotation

under the same dataset, `nlist`, `nprobe`, `bits`, `topk`, and query count.

#### Scenario: MSMARCO operating point is compared under two rotation modes
- **WHEN** the padded-Hadamard experiment is run on MSMARCO
- **THEN** the exported results MUST include one baseline run and one padded-Hadamard run
- **AND** both runs MUST use the same `nlist`, `nprobe`, `bits`, `topk`, and query count

### Requirement: The experiment workflow SHALL gate full E2E comparison on prepare/rotation microbench evidence
The experiment workflow MUST include a microbenchmark or equivalent prepare-focused comparison before interpreting the full E2E result as a serving-path decision.

#### Scenario: Microbench result precedes full E2E decision
- **WHEN** the padded-Hadamard candidate is evaluated
- **THEN** the workflow MUST record a prepare/rotation-focused comparison before or alongside the full E2E comparison
- **AND** the final decision MUST be attributable to both prepare-level and full-query measurements

### Requirement: The experiment workflow SHALL report the tradeoff between faster rotation and larger effective dimension
The experiment workflow MUST report both latency and footprint consequences of moving from `logical_dim` to `effective_dim`.

#### Scenario: Result export includes both timing and footprint fields
- **WHEN** a padded-Hadamard experiment result is exported
- **THEN** it MUST include recall and query latency fields
- **AND** it MUST include `logical_dim`, `effective_dim`, and index-size or artifact-size fields
