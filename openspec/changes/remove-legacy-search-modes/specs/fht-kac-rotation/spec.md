## MODIFIED Requirements

### Requirement: IVF-RaBitQ SHALL support an `fht_kac_rotator` rotation mode
The system SHALL use `fht_kac_rotator` as the only formal rotation mode for non-power-of-two dimensions. This mode SHALL remain identifiable after reopen and SHALL not require a user-facing opt-in flag for mainline builds.

#### Scenario: Builder selects FhtKac rotation for non-power-of-two dimensions
- **WHEN** a formal index build uses a non-power-of-two logical dimension
- **THEN** the builder SHALL record `rotation_mode = "fht_kac_rotator"`
- **AND** it SHALL persist enough metadata to reconstruct the same rotation on reopen

#### Scenario: Reopen preserves rotation mode identity
- **WHEN** an index built with `fht_kac_rotator` is reopened
- **THEN** the index SHALL expose `rotation_mode = "fht_kac_rotator"`
- **AND** the reopened rotation SHALL produce the same output as the saved rotation for the same input vector

#### Scenario: Non-power-of-two build does not fall back to random or blocked rotation
- **WHEN** a non-power-of-two formal index is built without legacy rotation options
- **THEN** the builder SHALL NOT use `random_matrix`, `hadamard_padded`, or `blocked_hadamard_permuted`
- **AND** it SHALL use `fht_kac_rotator`

### Requirement: FhtKac rotation SHALL only require padding to a multiple of 64
The FhtKac rotation mode SHALL define `effective_dim` as the smallest multiple of `64` that is greater than or equal to `logical_dim`, rather than the next power of two.

#### Scenario: 768-dimensional vectors keep their effective dimension
- **WHEN** the logical dimension is `768`
- **THEN** the FhtKac rotation mode SHALL keep `effective_dim = 768`
- **AND** it SHALL NOT expand the dimension to `1024`

#### Scenario: Non-64-multiple dimensions are rounded up minimally
- **WHEN** the logical dimension is not a multiple of `64`
- **THEN** the FhtKac rotation mode SHALL set `effective_dim` to the smallest greater-or-equal multiple of `64`

### Requirement: FhtKac rotation SHALL remain compatible with the query-once rotated-centroid fast path
The system SHALL treat `fht_kac_rotator` as the formal pre-rotated query path for non-power-of-two dimensions. It SHALL generate `rotated_centroids.bin` at build time and SHALL allow query-time execution to rotate each query once and reuse `PrepareQueryRotatedInto`.

#### Scenario: Build produces rotated centroids for FhtKac
- **WHEN** an index is built with `fht_kac_rotator`
- **THEN** the build output SHALL include rotated centroid data aligned with the FhtKac rotation mode

#### Scenario: Query path rotates the query only once
- **WHEN** a query is executed against an index built with `fht_kac_rotator`
- **THEN** the query pipeline SHALL rotate the query once before probing
- **AND** it SHALL reuse the pre-rotated centroid path rather than re-rotating the residual for every cluster
