## ADDED Requirements

### Requirement: Blocked Hadamard rotation SHALL NOT be a formal build or query mode
The system SHALL NOT expose `blocked_hadamard_permuted` as a formal rotation mode for new IVF-RaBitQ builds or benchmark runs.

#### Scenario: Build rejects blocked Hadamard mode
- **WHEN** a caller requests `blocked_hadamard_permuted` for a new formal build
- **THEN** the builder SHALL reject the request with a clear unsupported-mode error
- **AND** it SHALL NOT silently choose blocked Hadamard for non-power-of-two dimensions

#### Scenario: Formal benchmark does not emit blocked Hadamard variants
- **WHEN** benchmark scripts generate formal rotation experiments
- **THEN** they SHALL NOT emit `--blocked-hadamard-permuted`
- **AND** they SHALL use FHT-Kac for non-power-of-two dimensions

## REMOVED Requirements

### Requirement: Builder 支持非幂次维度下的 blocked Hadamard permuted 构建
**Reason**: Non-power-of-two formal builds now use FHT-Kac only.
**Migration**: Rebuild blocked-Hadamard indexes with FHT-Kac.

### Requirement: Blocked Hadamard 旋转可持久化并可重开复现
**Reason**: Persisting and reopening blocked-Hadamard metadata is no longer required for formal serving.
**Migration**: Legacy metadata may be identified for error reporting, but formal serving SHALL use FHT-Kac indexes.

### Requirement: Blocked Hadamard 参与旋转查询快路径
**Reason**: Query-once rotation support is retained for Hadamard and FHT-Kac only.
**Migration**: Use FHT-Kac pre-rotated centroid artifacts for non-power-of-two dimensions.
