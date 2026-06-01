## ADDED Requirements

### Requirement: CRC early-stop SHALL be independent from dynamic SafeOut
CRC early-stop SHALL only control whether the probe loop may terminate early via `CrcStopper::ShouldStop()`. Dynamic SafeOut frontier maintenance and SafeOut candidate pruning SHALL NOT require CRC calibration parameters, `CrcStopper`, or probe-loop early stopping to be enabled.

#### Scenario: CRC disabled but dynamic SafeOut enabled
- **WHEN** query configuration disables CRC early-stop or provides no `crc_params`
- **AND** dynamic SafeOut is enabled
- **THEN** runtime MUST NOT call `CrcStopper::ShouldStop()`
- **AND** runtime MUST still maintain the dynamic SafeOut frontier state
- **AND** runtime MUST allow SafeOut pruning once the dynamic SafeOut frontier state is full

#### Scenario: early_stop disabled but dynamic SafeOut enabled
- **WHEN** query configuration sets `early_stop=false`
- **AND** dynamic SafeOut is enabled
- **THEN** runtime MUST NOT break out of the probe loop due to CRC
- **AND** runtime MUST still maintain and apply dynamic SafeOut frontier pruning

#### Scenario: CRC enabled but dynamic SafeOut disabled
- **WHEN** CRC early-stop is enabled
- **AND** dynamic SafeOut is explicitly disabled
- **THEN** runtime MUST maintain the CRC kth-score state required by `CrcStopper`
- **AND** runtime MUST NOT use that CRC state to prune candidates as SafeOut

#### Scenario: CRC score state remains calibration-compatible
- **WHEN** CRC early-stop is enabled
- **THEN** the `current_kth_dist` passed to `CrcStopper::ShouldStop()` MUST come from a state whose score semantics match the CRC calibration scores
- **AND** runtime MUST NOT replace the CRC kth score with `kth(d_hat + e)` unless CRC calibration is explicitly changed to the same score space
