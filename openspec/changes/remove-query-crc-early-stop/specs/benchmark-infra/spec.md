## ADDED Requirements

### Requirement: Formal benchmarks SHALL not expose probing CRC as an active search mode
Formal vector-search and E2E benchmarks SHALL use fixed-`nprobe` query execution and SHALL NOT expose probing CRC early-stop as an active search mode.

#### Scenario: CRC CLI flags do not enable search behavior
- **WHEN** a benchmark binary is run after this change
- **THEN** `--crc`, `--early-stop`, and probing `--crc-*` flags MUST be removed or treated as deprecated no-ops
- **AND** those flags MUST NOT change cluster probing count, SafeOut behavior, or recall semantics

#### Scenario: benchmark output has no active CRC early-stop metrics
- **WHEN** benchmark writes JSON or summary output
- **THEN** the output MUST NOT report CRC early-stop as an active enabled feature
- **AND** any retained compatibility fields MUST be explicitly fixed/deprecated and MUST NOT be used for conclusions

#### Scenario: default benchmark does not require CRC files
- **WHEN** benchmark runs on an index directory without `crc_scores.bin`
- **THEN** the benchmark MUST still run vector search and E2E search if all non-CRC required files are present
- **AND** missing CRC files MUST NOT disable dynamic SafeOut

### Requirement: CRC early-stop tests and tools SHALL leave the default validation matrix
Tests and tools whose only purpose is probing CRC early-stop SHALL be removed from the default build/test path.

#### Scenario: default test suite excludes CRC stopper tests
- **WHEN** developers run the default CTest/build validation
- **THEN** tests that only validate `CrcStopper::ShouldStop()` or probing CRC early-stop MUST NOT be required
- **AND** tests for fixed-`nprobe` search, SafeOut independence, and result correctness MUST remain required

#### Scenario: legacy diagnostic tools are isolated
- **WHEN** historical CRC diagnostic binaries are retained
- **THEN** they MUST be clearly marked legacy/diagnostic
- **AND** they MUST NOT be built or invoked by the formal benchmark workflow by default
