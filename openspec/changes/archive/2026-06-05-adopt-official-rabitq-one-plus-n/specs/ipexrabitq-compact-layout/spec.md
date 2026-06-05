## ADDED Requirements

### Requirement: Compact layout SHALL support official 1+n blocked ExData
系统 SHALL 为 official `1+n` ExRaBitQ 定义新的 compact blocked Region2 layout。该 layout MUST 复用 batch-major Stage2 block organization，但 payload MUST 是 sign-folded ExData，而不是 legacy `magnitude + sign`。

#### Scenario: Official batch block stores sign-folded ExData
- **WHEN** writer 序列化 official `1+n` Stage2 batch block
- **THEN** block 必须记录 valid lane count
- **AND** 每个 lane/dim-block 必须存储按 `ex_bits` bit-pack 的 sign-folded ExData code
- **AND** block 不得包含独立 sign block

#### Scenario: Official layout remains batch-addressable
- **WHEN** query scheduler 访问一个 Stage2 batch block
- **THEN** 它必须能按 batch block id 直接定位 official ExData payload
- **AND** 不得在 hot path 将 per-vector payload repack 成 batch-major layout

### Requirement: Official ExData layout SHALL support ex_bits three
official compact layout MUST support `ex_bits=3` because `total_bits=4` maps to official `1+3` RaBitQ. 3-bit ExData MAY be represented as a low 2-bit compact part plus a high 1-bit compact part, or another documented bit-exact layout.

#### Scenario: Total bits four maps to three-bit ExData
- **WHEN** 构建器使用 `total_bits=4`
- **THEN** storage 必须设置 `ex_bits=3`
- **AND** Region2 必须使用 3-bit ExData packing
- **AND** benchmark metadata 必须报告 `total_bits=4` 和 `ex_bits=3`

#### Scenario: Three-bit layout round trips exactly
- **WHEN** writer pack 一组 `ex_bits=3` ExData code 并由 reader unpack
- **THEN** unpack 后每个 code 必须与写入前一致
- **AND** tail dimensions and padded lanes 必须保持正确

### Requirement: Resident preload SHALL keep official ExData compact
resident preload SHALL retain official ExData in compact packed form and SHALL NOT materialize a full-index decoded ExData copy as resident state.

#### Scenario: Resident official index avoids full decoded ExData
- **WHEN** official `1+n` index 完成 compact resident preload
- **THEN** resident state 必须保留 compact ExData code storage
- **AND** 不得保留全索引 decoded `uint8` ExData 副本
- **AND** memory metrics 必须报告 official ExData code storage bytes

#### Scenario: Query may decode touched blocks to scratch
- **WHEN** 查询访问一个 packed official Stage2 block
- **THEN** runtime 可以将该 touched block 临时 decode 到 scratch
- **AND** scratch 生命周期不得超过 query/thread-local execution scope
