## REMOVED Requirements

### Requirement: CRC calibration is consumed by online probing early-stop
**Reason**: probing early-stop is no longer part of the target search method, so online query SHALL NOT depend on `CalibrationResults` for stop decisions.

**Migration**: Existing `crc_scores.bin`, `crc_calibration_params.bin`, and `segment.meta.crc_params` may remain on disk for compatibility, but formal query and benchmark paths SHALL ignore them.

#### Scenario: online query ignores CRC calibration artifacts
- **WHEN** an index directory contains `crc_scores.bin` or `crc_calibration_params.bin`
- **THEN** the query runtime MUST NOT load those artifacts for probing early-stop
- **AND** the absence of those artifacts MUST NOT prevent vector search or SafeOut pruning

#### Scenario: benchmark does not perform runtime CRC calibration
- **WHEN** `bench_vector_search` or `bench_e2e` prepares a query run
- **THEN** the benchmark MUST NOT run CRC calibration as a required phase
- **AND** the benchmark MUST NOT fail solely because probing CRC calibration artifacts are missing

## ADDED Requirements

### Requirement: CRC storage compatibility SHALL be preserved
The system SHALL preserve read compatibility for indexes whose metadata still contains CRC references.

#### Scenario: old index with CRC metadata opens under new runtime
- **WHEN** runtime opens an index whose `segment.meta` includes `crc_params`
- **THEN** index opening MUST succeed if all non-CRC required index files are valid
- **AND** the runtime MUST ignore CRC metadata for query execution

#### Scenario: new index omits probing CRC score generation
- **WHEN** builder creates a new index for the fixed-`nprobe` query path
- **THEN** builder MUST NOT require calibration queries for probing CRC score precomputation
- **AND** builder MUST NOT generate `crc_scores.bin` for formal query early-stop use
