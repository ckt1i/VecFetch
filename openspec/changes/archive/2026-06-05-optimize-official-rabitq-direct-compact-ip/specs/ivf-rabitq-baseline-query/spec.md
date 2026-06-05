## ADDED Requirements

### Requirement: IVF-RaBitQ query SHALL use optimized direct compact official Stage2 when selected
The IVF-RaBitQ baseline query path SHALL use the selected optimized direct compact official Stage2 kernel when querying selected-layout `total_bits=4, ex_bits=3` indexes.

#### Scenario: Selected optimized index routes to selected kernel
- **WHEN** a query run opens a selected optimized official `1+3` index
- **THEN** Stage2 MUST use the selected direct compact kernel
- **AND** it MUST preserve official `1+n` score combination with Stage1 `ip_x0_qr`

#### Scenario: Query outputs expose direct compact execution
- **WHEN** a query run completes on an optimized official `1+3` index
- **THEN** the output metadata MUST report the compact layout key, selected status, average Stage2 time, average rerank count, and Stage2 decode counters
- **AND** the output MUST be sufficient to verify that full-block decode was bypassed

### Requirement: IVF-RaBitQ query SHALL preserve fallback and validation paths
The query path SHALL preserve the ability to run generic v13 official decode-to-scratch indexes for fallback and correctness comparison.

#### Scenario: Generic v13 query still works
- **WHEN** a query run opens a generic v13 official `1+3` index
- **THEN** the query path MAY use the existing decode-to-scratch implementation
- **AND** output metadata MUST report that the optimized direct compact path was not used

#### Scenario: Direct compact and fallback are compared with identical controls
- **WHEN** correctness or performance comparison is reported
- **THEN** the optimized direct compact query and fallback query MUST use the same query set, GT file, `nprobe`, `topk`, and candidate budget
- **AND** result differences MUST be attributed to the layout/kernel rather than search parameter changes
