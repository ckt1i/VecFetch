## ADDED Requirements

### Requirement: E2E benchmark CLI SHALL remove legacy search-mode knobs
`bench_e2e` SHALL remove or reject formal use of legacy mode knobs for HNSW coarse routing, redundant/RAIR assignment, padded Hadamard, and blocked Hadamard.

#### Scenario: Removed routing knobs are not active
- **WHEN** a user invokes `bench_e2e` with `--hnsw-coarse-routing` or related HNSW parameters
- **THEN** the benchmark SHALL reject the option with a clear error or document it as unsupported compatibility input
- **AND** the measured run SHALL NOT use HNSW coarse routing

#### Scenario: Removed assignment knobs are not active
- **WHEN** a user invokes `bench_e2e` with `--assignment-mode`, `--assignment-factor`, `--rair-lambda`, `--rair-strict-second-choice`, or secondary assignment output options
- **THEN** the benchmark SHALL reject the option with a clear error or document it as unsupported compatibility input
- **AND** the measured run SHALL use a single-assignment index

#### Scenario: Removed rotation knobs are not active
- **WHEN** a user invokes `bench_e2e` with `--pad-to-pow2` or `--blocked-hadamard-permuted`
- **THEN** the benchmark SHALL reject the option with a clear error or document it as unsupported compatibility input
- **AND** non-power-of-two builds SHALL use FHT-Kac

### Requirement: E2E benchmark output SHALL report only mainline mode metadata
`bench_e2e` output SHALL keep metadata required to reproduce the mainline run and SHALL remove fields that imply removed modes are active tuning dimensions.

#### Scenario: Output omits removed mode fields
- **WHEN** `bench_e2e` writes JSON or CSV output
- **THEN** the result SHALL NOT report HNSW-specific routing fields, RAIR-specific parameters, secondary assignment fields, or blocked/padded Hadamard flags as active configuration
- **AND** it SHALL still report dataset, index path, `nlist`, `nprobe`, `topk`, `bits`, metric, rotation mode, recall, latency, and SafeIn/SafeOut/Uncertain statistics

#### Scenario: Rotation mode remains explicit
- **WHEN** `bench_e2e` writes result metadata for a non-power-of-two index
- **THEN** `rotation_mode` SHALL be `fht_kac_rotator`
- **AND** the output SHALL make this mainline mode visible

## REMOVED Requirements

### Requirement: E2E benchmark SHALL compare single, naive top-2, and RAIR top-2 under one protocol
**Reason**: Redundant and RAIR assignment modes are removed from the formal benchmark matrix.
**Migration**: Run the single-assignment mainline benchmark.

### Requirement: Benchmark evaluation SHALL answer whether RAIR reduces high-recall probing demand
**Reason**: RAIR is no longer a supported algorithmic branch for high-recall probing reduction.
**Migration**: Evaluate high-recall behavior under the single-assignment mainline.

### Requirement: Benchmark output SHALL expose the cost of RAIR-based redundant serving
**Reason**: RAIR redundant serving costs are no longer part of formal output.
**Migration**: Output mainline resident single-assignment costs only.
