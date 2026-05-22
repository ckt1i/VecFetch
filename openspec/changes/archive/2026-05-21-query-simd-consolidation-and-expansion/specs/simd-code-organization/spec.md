## ADDED Requirements

### Requirement: SIMD implementation code SHALL be centralized under the simd directories
All SIMD implementation code introduced or maintained for the query path SHALL reside under `src/simd` and `include/vdb/simd`. Business-layer modules such as `src/index/*` and `src/query/*` MUST NOT embed new ISA-specific implementation logic directly.

Business-layer modules MAY retain:

- feature gates
- scratch preparation
- scalar result assembly
- calls into `simd::*` interfaces

They MUST NOT remain the long-term home for AVX/ISA-specific kernels.

#### Scenario: New SIMD kernel is added
- **WHEN** a new query-path SIMD kernel is introduced
- **THEN** its implementation MUST be placed under `src/simd`
- **AND** its public declaration MUST be placed under `include/vdb/simd`

#### Scenario: Existing embedded SIMD helper is cleaned up
- **WHEN** an existing query or index module contains embedded SIMD implementation logic
- **THEN** that logic MUST be migrated into the simd directories
- **AND** the original module MUST be reduced to call-site and control-flow responsibilities

### Requirement: SIMD organization SHALL preserve explicit fallback paths
Every SIMD capability moved into the simd directories SHALL preserve an explicit fallback path that can be selected by the caller or configuration. The fallback path MUST remain available for correctness comparison and rollback.

#### Scenario: SIMD capability is turned off
- **WHEN** a caller disables a SIMD capability
- **THEN** the business-layer module MUST be able to call the scalar or reference fallback
- **AND** that fallback MUST not require a different result consumer contract

### Requirement: SIMD modules SHALL expose stable business-facing interfaces
Each SIMD module SHALL expose a stable business-facing interface that hides ISA-specific details from the caller. Callers MUST interact with the SIMD module through named helper functions rather than open-coded AVX branches.

For newly introduced query runtime paths, the interface MUST be established in Phase 1 and preserved through any later Phase 2 specialization so that the business-layer call site does not need to change when stronger SIMD implementations are added.

#### Scenario: Business module calls a SIMD helper
- **WHEN** a query or index module uses a SIMD capability
- **THEN** it MUST call through a declared `simd::*` interface
- **AND** the caller MUST not need to manage ISA-specific register-level behavior directly

#### Scenario: Runtime path upgrades from Phase 1 to Phase 2
- **WHEN** a newly introduced SIMD runtime path adds a stronger specialization after its initial rollout
- **THEN** the business-layer module MUST continue to call the same stable `simd::*` interface
- **AND** the stronger specialization MUST remain an internal concern of the simd module
