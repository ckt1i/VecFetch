## ADDED Requirements

### Requirement: Full preload SHALL keep packed Stage2 magnitudes resident in packed form
当 full `.clu` preload 用于 packed Stage2 magnitude index 时，resident state MUST 保持 Stage2 magnitude bytes 为 packed form，并且 MUST NOT 要求 full-index decoded magnitude materialization。

#### Scenario: Packed Stage2 index 被 preload
- **当** full `.clu` preload 读取 packed Stage2 magnitude index
- **则** resident `.clu` buffer 必须包含 packed Stage2 magnitude bytes
- **并且** resident parsed cluster views 必须直接引用 packed Stage2 magnitude bytes

#### Scenario: Preload 避免 full decoded Stage2 resident copy
- **当** full `.clu` preload 完成 packed Stage2 magnitude index 的加载
- **则** resident memory accounting 不得包含 all-index decoded Stage2 magnitude copy
- **并且** 任何 decoded magnitude scratch 必须限定在 query-time touched blocks 范围内

### Requirement: Full preload metrics SHALL distinguish packed resident bytes from scratch decode bytes
Benchmark 和 preload metrics MUST 区分持久 resident packed Stage2 memory 与临时 query decode scratch，以便验证内存收益。

#### Scenario: bench_e2e 报告 resident packed memory
- **当** `bench_e2e` 在 full preload 模式下运行 packed Stage2 magnitude index
- **则** result metadata 必须报告 resident `.clu` preload bytes
- **并且** 它必须报告 resident Stage2 magnitude mirror 是否被避免
- **并且** 它必须在可用时报告 temporary decode scratch capacity 或等价 bound
