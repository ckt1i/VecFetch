## REMOVED Requirements

### Requirement: RaBitQ CRC calibration drives probing early-stop
**Reason**: RaBitQ-based CRC calibration was introduced to align CRC stop decisions with online RaBitQ distance estimates. Since probing early-stop is being removed, this calibration path no longer has a production query consumer.

**Migration**: Remove the production wiring, tests, and benchmark dependencies for RaBitQ CRC probing early-stop. Candidate-level SafeIn calibration is out of scope and SHALL NOT be removed by this requirement.

#### Scenario: RaBitQ CRC calibration is not required for search
- **WHEN** vector search runs with RaBitQ estimates and dynamic SafeOut enabled
- **THEN** search MUST NOT require RaBitQ CRC calibration results
- **AND** search MUST NOT instantiate CRC calibration-derived stopping logic

#### Scenario: RaBitQ CRC tests are removed from the formal test matrix
- **WHEN** the default test suite is run
- **THEN** tests that only validate probing CRC early-stop calibration MUST NOT be required
- **AND** remaining RaBitQ/SafeOut/SafeIn tests MUST continue to cover active query behavior

#### Scenario: candidate-level SafeIn calibration remains separate
- **WHEN** implementation removes RaBitQ CRC probing artifacts
- **THEN** code paths used for candidate-level SafeIn threshold calibration MUST remain available unless a separate change explicitly replaces them
