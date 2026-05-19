## ADDED Requirements

### Requirement: Benchmark SHALL accept an external ground-truth file
The benchmark SHALL allow callers to provide a precomputed ground-truth file for recall evaluation instead of always generating ground truth internally at runtime.

#### Scenario: External GT file is provided
- **WHEN** the user runs `bench_e2e` with an explicit ground-truth file argument
- **THEN** the benchmark SHALL load the provided file and use it for recall computation
- **AND** it SHALL NOT recompute brute-force ground truth for that run

#### Scenario: External GT file is omitted
- **WHEN** the user does not provide a ground-truth file argument
- **THEN** the benchmark SHALL retain its existing behavior for ground-truth generation or skipping

### Requirement: Benchmark SHALL validate the external ground-truth contract
The benchmark SHALL validate the shape and size of the external ground-truth data before it is used.

#### Scenario: Ground truth shape matches queries
- **WHEN** the provided ground-truth file is loaded successfully
- **THEN** the benchmark SHALL verify that the number of ground-truth rows matches the number of queries being evaluated

#### Scenario: Ground truth is malformed
- **WHEN** the provided ground-truth file cannot be parsed or has an incompatible shape
- **THEN** the benchmark SHALL fail with a clear error message before query execution begins

### Requirement: Benchmark output SHALL record whether external ground truth was used
The benchmark SHALL record whether recall metrics were computed from an externally supplied ground-truth file.

#### Scenario: External GT is active
- **WHEN** the benchmark uses an external ground-truth file
- **THEN** the output metrics SHALL record that external GT was used
- **AND** the run metadata SHALL include the GT source path

