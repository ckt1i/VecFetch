## ADDED Requirements

### Requirement: IVF-RaBitQ SHALL support an `fht_kac_rotator` rotation mode
The system SHALL provide a distinct `fht_kac_rotator` rotation mode for non-power-of-two dimensions. This mode SHALL be selectable during index build and SHALL remain identifiable after reopen.

#### Scenario: Builder selects FhtKac rotation
- **WHEN** the build configuration enables `fht_kac_rotator`
- **THEN** the builder SHALL record `rotation_mode = "fht_kac_rotator"`
- **AND** it SHALL persist enough metadata to reconstruct the same rotation on reopen

#### Scenario: Reopen preserves rotation mode identity
- **WHEN** an index built with `fht_kac_rotator` is reopened
- **THEN** the index SHALL expose `rotation_mode = "fht_kac_rotator"`
- **AND** the reopened rotation SHALL produce the same output as the saved rotation for the same input vector

### Requirement: FhtKac rotation SHALL only require padding to a multiple of 64
The FhtKac rotation mode SHALL define `effective_dim` as the smallest multiple of `64` that is greater than or equal to `logical_dim`, rather than the next power of two.

#### Scenario: 768-dimensional vectors keep their effective dimension
- **WHEN** the logical dimension is `768`
- **THEN** the FhtKac rotation mode SHALL keep `effective_dim = 768`
- **AND** it SHALL NOT expand the dimension to `1024`

#### Scenario: Non-64-multiple dimensions are rounded up minimally
- **WHEN** the logical dimension is not a multiple of `64`
- **THEN** the FhtKac rotation mode SHALL set `effective_dim` to the smallest greater-or-equal multiple of `64`

### Requirement: FhtKac rotation SHALL apply repeated sign-flip, FFHT, and Kac mixing rounds
The FhtKac rotation mode SHALL implement a deterministic sequence of rounds composed of sign flipping, alternating FFHT on a `trunc_dim = floor_power_of_two(effective_dim)` window, and Kac/Givens-style half mixing.

#### Scenario: FhtKac rotation alternates FFHT windows for non-power-of-two dimensions
- **WHEN** `effective_dim` is not a power of two
- **THEN** the rotation SHALL alternate the FFHT window between the front and back `trunc_dim` coordinates across rounds
- **AND** it SHALL apply a half-mixing step after each round

#### Scenario: FhtKac rotation is deterministic for a fixed seed
- **WHEN** the same dimension and seed are used
- **THEN** the system SHALL generate the same sign sequences and the same rotated output for the same input vector

### Requirement: FhtKac rotation SHALL remain compatible with the query-once rotated-centroid fast path
The system SHALL treat `fht_kac_rotator` as an eligible pre-rotated query path. It SHALL generate `rotated_centroids.bin` at build time and SHALL allow query-time execution to rotate each query once and reuse `PrepareQueryRotatedInto`.

#### Scenario: Build produces rotated centroids for FhtKac
- **WHEN** an index is built with `fht_kac_rotator`
- **THEN** the build output SHALL include rotated centroid data aligned with the FhtKac rotation mode

#### Scenario: Query path rotates the query only once
- **WHEN** a query is executed against an index built with `fht_kac_rotator`
- **THEN** the query pipeline SHALL rotate the query once before probing
- **AND** it SHALL reuse the pre-rotated centroid path rather than re-rotating the residual for every cluster
