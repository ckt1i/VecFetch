## ADDED Requirements

### Requirement: Dynamic SafeOut SHALL be independent from probing CRC
Dynamic SafeOut SHALL operate without CRC calibration parameters, CRC score files, or CRC early-stop configuration.

#### Scenario: SafeOut works when CRC params are absent
- **WHEN** `SearchConfig` has dynamic SafeOut enabled and no CRC parameters
- **THEN** the query pipeline MUST still maintain the SafeOut frontier
- **AND** candidate classification MUST still be able to produce SafeOut according to the dynamic frontier

#### Scenario: SafeOut frontier does not reuse CRC heap state
- **WHEN** implementation removes CRC-specific estimate heap state
- **THEN** dynamic SafeOut MUST retain its own frontier state or equivalent query-time kth upper-bound state
- **AND** SafeOut pruning MUST NOT depend on `CrcStopper`, `CalibrationResults`, or CRC merge counters

#### Scenario: fixed-nprobe search still reports SafeOut classification
- **WHEN** fixed-`nprobe` vector search completes
- **THEN** benchmark output MUST still report SafeIn/SafeOut/Uncertain counts for active classification stages
- **AND** those counts MUST be computed without requiring `--crc`
