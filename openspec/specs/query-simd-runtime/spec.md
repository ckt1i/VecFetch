## ADDED Requirements

### Requirement: Query path SHALL support independently gated SIMD runtime kernels
The query path SHALL support independently enableable SIMD runtime kernels for query-stage hot-path work that remains CPU-bound after the existing Stage1/Stage2 optimization round. These kernels MUST be independently switchable and MUST preserve the same observable ranking and candidate funnel semantics as the scalar fallback path.

The initial SIMD runtime scope SHALL cover:

- address decode batch processing
- rerank batched exact distance
- coarse small-topn selection
- Stage2 classify helpers that remain on the query path

For newly introduced runtime kernels that are not simple migrations of existing SIMD helpers, implementation SHALL proceed in two phases:

- Phase 1: stable interface + conservative implementation + explicit fallback
- Phase 2: stronger SIMD specialization behind the same interface

#### Scenario: SIMD runtime kernel is enabled
- **WHEN** a query-stage SIMD capability is enabled for a benchmark or serving run
- **THEN** the system MUST execute the corresponding SIMD kernel rather than the scalar fallback
- **AND** the externally visible recall and result ordering semantics MUST remain unchanged

#### Scenario: SIMD runtime kernel is disabled
- **WHEN** a query-stage SIMD capability is disabled
- **THEN** the system MUST fall back to the existing scalar or pre-change implementation
- **AND** the behavior MUST remain functionally equivalent to the SIMD-enabled path

#### Scenario: Newly introduced SIMD runtime kernel enters Phase 1
- **WHEN** a new query-path SIMD runtime kernel is first introduced
- **THEN** it MUST first provide a stable business-facing interface and a conservative implementation
- **AND** it MUST provide an explicit fallback path before stronger specialization is added

#### Scenario: Newly introduced SIMD runtime kernel enters Phase 2
- **WHEN** a query-path SIMD runtime kernel advances beyond its initial implementation
- **THEN** stronger SIMD specialization MUST remain behind the same business-facing interface
- **AND** the Phase 1 or scalar fallback path MUST remain available for comparison and rollback

### Requirement: Address decode SHALL provide a two-phase batch runtime path
The query pipeline SHALL provide a batch-oriented address decode runtime path that is callable from query-time candidate decoding. The batch runtime path MUST be usable as a drop-in replacement for the existing batch interface and MUST preserve decoded address semantics.

Phase 1 MUST provide the batch interface, a conservative implementation, and an explicit fallback.  
Phase 2 MAY replace the internal implementation with stronger SIMD specialization, but MUST keep the same interface and decoded-address semantics.

#### Scenario: Batch address decode is used for candidate decoding
- **WHEN** the query path decodes a batch of candidate vector addresses
- **THEN** it MUST be able to route decoding through the dedicated batch runtime kernel
- **AND** the decoded addresses MUST match the scalar reference path

#### Scenario: Address decode remains in Phase 1
- **WHEN** address decode has only completed its initial runtime-path implementation
- **THEN** the system MUST still expose the SIMD-facing batch interface from the simd directory
- **AND** correctness comparison against the scalar reference path MUST remain possible

#### Scenario: Address decode advances to Phase 2
- **WHEN** address decode adds stronger SIMD specialization
- **THEN** the specialized path MUST remain selectable behind the same interface
- **AND** Phase 1 or scalar fallback MUST remain available for rollback

### Requirement: Rerank SHALL support a two-phase batched exact-distance runtime kernel
The rerank stage SHALL support batched exact-distance kernels that process one query against multiple candidates in a single runtime call. The batched kernel MUST preserve the same exact-distance results used by the scalar rerank path.

Phase 1 MUST provide the batched kernel interface and an initial implementation callable from `RerankConsumer` without requiring a full consumer lifecycle rewrite.  
Phase 2 MAY replace the internal implementation with stronger SIMD specialization, but MUST preserve the same exact-distance semantics and caller contract.

#### Scenario: Batched rerank distance is enabled
- **WHEN** rerank batched distance is enabled
- **THEN** the rerank stage MUST be able to compute exact distances for multiple buffered candidates in one kernel invocation
- **AND** the final top-k results MUST remain equivalent to the scalar rerank path

#### Scenario: Rerank remains in Phase 1
- **WHEN** rerank SIMD has only completed its initial batched-distance implementation
- **THEN** the rerank stage MUST be able to consume the batched kernel without changing final result semantics
- **AND** the fallback path MUST remain available for correctness comparison

#### Scenario: Rerank advances to Phase 2
- **WHEN** rerank batched distance adds stronger specialization
- **THEN** the stronger specialization MUST remain behind the same batched-distance interface
- **AND** the final top-k results MUST remain equivalent to the Phase 1 and scalar paths

### Requirement: Coarse selection SHALL support a two-phase small-topn runtime path
The coarse cluster selection stage SHALL support a specialized runtime path for small `nprobe` operating points such as `64` and `256`. The specialized path MUST preserve the same selected cluster ordering semantics as the reference implementation.

Phase 1 MUST provide a small-topn runtime interface and an initial specialized path suitable for the primary operating points.  
Phase 2 MAY replace the internal implementation with stronger vectorized or block-specialized selection, but MUST keep the same external contract and selected-cluster semantics.

#### Scenario: Small-topn runtime path is enabled
- **WHEN** the query uses a supported small `nprobe` operating point and the specialized path is enabled
- **THEN** the system MUST be able to select the top coarse clusters through the dedicated runtime path
- **AND** the resulting cluster set MUST remain equivalent to the reference coarse selection path

#### Scenario: Coarse small-topn remains in Phase 1
- **WHEN** coarse small-topn has only completed its initial specialized runtime path
- **THEN** the business layer MUST be able to call that path through the simd interface
- **AND** fallback to the reference coarse selection path MUST remain available

#### Scenario: Coarse small-topn advances to Phase 2
- **WHEN** coarse small-topn adds stronger specialization
- **THEN** the stronger specialization MUST remain selectable behind the same interface
- **AND** the resulting selected cluster set MUST remain equivalent to the Phase 1 and reference paths
