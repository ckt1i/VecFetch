## ADDED Requirements

### Requirement: Selectable Epsilon Sampling Mode
Benchmark tools SHALL support selecting the epsilon calibration sampling mode without changing existing default behavior.

#### Scenario: Default calibration mode
- **WHEN** a benchmark requests SafeIn or SafeOut epsilon percentile calibration without specifying an epsilon sampling mode
- **THEN** the system SHALL use the existing per-cluster calibration behavior

#### Scenario: Global pair calibration mode
- **WHEN** a benchmark is run with `--epsilon-sampling-mode global_pair`
- **THEN** the system SHALL interpret `--epsilon-samples K` as a total target of K sampled query-target pairs

### Requirement: Global Pair Sampling Semantics
Global pair epsilon calibration SHALL estimate epsilon from randomly sampled in-cluster query-target pairs weighted by global row frequency.

#### Scenario: Sampled pair contributes one error
- **WHEN** global pair calibration samples a valid query-target pair
- **THEN** the system SHALL compute one true distance, one estimated RabitQ distance, and one normalized error contribution

#### Scenario: Invalid pair retry
- **WHEN** a sampled pair cannot produce a valid normalized error
- **THEN** the system SHALL retry sampling up to a bounded attempt limit and SHALL report the realized valid error count

### Requirement: Calibration Reporting
Benchmark outputs SHALL report enough epsilon calibration metadata to reproduce and compare runs.

#### Scenario: Results include sampling metadata
- **WHEN** epsilon percentile calibration runs
- **THEN** the output JSON SHALL include epsilon sampling mode, requested sample count, realized valid error count, percentile, and runtime epsilon value

### Requirement: Compatibility With Query Semantics
Fast epsilon calibration SHALL NOT change query-time ranking, SafeOut/SafeIn classification formulas, or index file formats.

#### Scenario: Runtime query behavior
- **WHEN** a benchmark uses global pair calibration to derive a SafeOut epsilon
- **THEN** the query pipeline SHALL use the derived epsilon through the existing SafeOut override path
