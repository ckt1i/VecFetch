## ADDED Requirements

### Requirement: Packed Stage2 compact layout SHALL store magnitudes in bit-width-coded blocks
`v11` 后继 compact Stage2 layout MUST 以 bit-packed 形式存储 batch-major Stage2 magnitude payload，同时保留现有 batch size、dimension block 策略、packed sign 和 `xipnorm` 值。

#### Scenario: Packed compact block 保持 batch-major 组织方式
- **当** builder 序列化 packed Stage2 compact block
- **则** 该 block 必须继续按 Stage2 batch 和 dimension block 组织
- **并且** 它必须为与 byte-magnitude compact format 相同的 lanes 保留 `valid_count`、packed signs 和 `xipnorm`

#### Scenario: Packed compact block 减少 magnitude bytes
- **当** builder 序列化完整的 8-lane、64-dimension sub-block
- **则** 持久化 magnitude byte count 必须为 `8 * ceil(64 * bits / 8)` 或等价 byte-aligned 表示
- **并且** 对 bits 2、4 不得写入 `8 * 64` 个 magnitude bytes

### Requirement: Packed compact layout SHALL expose decode metadata to query views
packed Stage2 compact block 的 parsed cluster view MUST 暴露 query-time decode 所需元数据，包括 active bit width、dimension block size、batch size、valid lane count，以及 packed magnitude、sign、`xipnorm` 区域指针。

#### Scenario: 已解析的 packed compact block 可以被查询路径解码
- **当** packed Stage2 compact block 从 resident memory 或 async block read 中被解析
- **则** 生成的 parsed view 必须包含 active bit width
- **并且** 它必须包含每个 Stage2 batch block 的 packed magnitude block location
- **并且** 它必须包含现有 packed sign 和 `xipnorm` locations

### Requirement: Packed compact layout SHALL avoid resident decoded magnitude mirrors
packed compact layout MUST NOT 要求在 preload 或 parse 阶段为所有 Stage2 blocks 构建完整 resident decoded magnitude mirror。

#### Scenario: Full preload 解析 packed compact blocks
- **当** full `.clu` preload 解析 packed Stage2 compact blocks
- **则** resident views 必须指向 packed magnitude bytes
- **并且** query execution 不得依赖 resident all-index `uint8_t` magnitude mirror
