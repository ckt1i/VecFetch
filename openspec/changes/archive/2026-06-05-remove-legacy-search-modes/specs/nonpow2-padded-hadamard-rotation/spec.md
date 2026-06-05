## ADDED Requirements

### Requirement: Padded Hadamard SHALL NOT be a formal non-power-of-two rotation mode
The system SHALL NOT expose padded Hadamard or `--pad-to-pow2` as a formal build or benchmark option for non-power-of-two dimensions.

#### Scenario: pad-to-pow2 option is removed
- **WHEN** a user invokes formal build or benchmark tooling
- **THEN** `--pad-to-pow2` SHALL NOT be accepted as an active tuning parameter
- **AND** non-power-of-two dimensions SHALL use FHT-Kac rather than next-power-of-two Hadamard padding

#### Scenario: non-power-of-two build does not produce padded Hadamard metadata
- **WHEN** a non-power-of-two formal index build completes
- **THEN** the output metadata SHALL NOT record `rotation_mode = "hadamard_padded"`
- **AND** it SHALL record `rotation_mode = "fht_kac_rotator"`

## REMOVED Requirements

### Requirement: Non-power-of-two dimensions SHALL support padded Hadamard rotation as an explicit experimental mode
**Reason**: FHT-Kac replaces padded Hadamard as the only formal non-power-of-two rotation path.
**Migration**: Rebuild padded-Hadamard indexes with FHT-Kac.

### Requirement: Padded Hadamard mode SHALL preserve build/query symmetry
**Reason**: Padded-Hadamard build/query symmetry is no longer required for formal serving.
**Migration**: FHT-Kac SHALL provide build/query symmetry for non-power-of-two dimensions.

### Requirement: Padded Hadamard mode SHALL persist rotated-centroid artifacts for the padded dimension
**Reason**: Formal rotated-centroid artifacts for non-power-of-two dimensions now come from FHT-Kac.
**Migration**: Use FHT-Kac rotated-centroid artifacts.
