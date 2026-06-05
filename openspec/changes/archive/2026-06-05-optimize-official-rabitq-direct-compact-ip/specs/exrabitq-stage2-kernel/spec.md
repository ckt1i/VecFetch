## ADDED Requirements

### Requirement: Official 3-bit Stage2 SHALL provide direct compact SIMD IP kernels
The Stage2 kernel layer SHALL provide direct compact IP kernels for official `ex_bits=3` ExData in both `2-bit + 1-bit` and `1-bit + 1-bit + 1-bit` layouts. These kernels MUST compute `ip_ex = dot(query, ex_code)` directly from compact bytes without first decoding an entire Stage2 block into a `uint8_t` scratch buffer.

#### Scenario: 2-plus-1 kernel computes selected lanes directly
- **WHEN** Stage2 receives a `2-bit + 1-bit` official ExData block and a block-local lane mask
- **THEN** the kernel MUST compute `ip_ex` for selected lanes directly from compact low-2-bit and high-1-bit payloads
- **AND** it MUST NOT call the generic full-block ExData decode routine for that block

#### Scenario: Three-bitplane kernel computes selected lanes directly
- **WHEN** Stage2 receives a `1-bit + 1-bit + 1-bit` official ExData block and a block-local lane mask
- **THEN** the kernel MUST compute `ip_ex` as `dot(q,b0) + 2*dot(q,b1) + 4*dot(q,b2)` for selected lanes
- **AND** it MUST NOT call the generic full-block ExData decode routine for that block

### Requirement: Direct compact kernels SHALL preserve official ExData correctness
Each direct compact official 3-bit kernel SHALL match the scalar sign-folded ExData reference within the established floating-point tolerance.

#### Scenario: Kernel parity against scalar reference
- **WHEN** randomized official 3-bit ExData codes, queries, dimensions, and lane masks are tested
- **THEN** every selected lane's direct compact `ip_ex` MUST match the scalar decoded-reference `ip_ex` within tolerance

#### Scenario: E2E parity against v13 official fallback
- **WHEN** an optimized direct compact index and an equivalent generic v13 official fallback index are queried with the same data, GT, `nprobe`, `topk`, and candidate budget
- **THEN** recall and final result semantics MUST match within the established floating-point tolerance

### Requirement: Stage2 official fast path SHALL select the winning direct compact kernel
After benchmark comparison, the official `ex_bits=3` Stage2 fast path SHALL use the selected direct compact kernel by default and SHALL keep the generic v13 decode path only as fallback or validation.

#### Scenario: Selected fast path bypasses generic decode
- **WHEN** query execution uses an optimized official `ex_bits=3` selected-layout index
- **THEN** Stage2 MUST route to the selected direct compact kernel
- **AND** average `stage2_decode_blocks` for the optimized path SHOULD be zero or explicitly marked as not applicable

#### Scenario: Generic decode remains available for fallback
- **WHEN** query execution uses a generic v13 official index or an unsupported optimized layout
- **THEN** the system MAY fall back to the existing decode-to-scratch path
- **AND** benchmark metadata MUST distinguish fallback execution from selected direct compact execution
