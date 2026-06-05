## ADDED Requirements

### Requirement: Stage2 kernel path SHALL support touched-block scratch decode
Stage2 query path MUST 支持对 packed-magnitude compact blocks 进行评估：仅将当前 query 实际访问的 Stage2 batch blocks 解码到可复用 scratch，然后通过 Stage2 SIMD kernel 消费该 scratch。

#### Scenario: 未访问的 Stage2 block 不会被解码
- **当** 某个 query 不需要对 packed-magnitude batch block 执行 Stage2 evaluation
- **则** 系统不得为该 query 解码这个 block 的 magnitude

#### Scenario: 已访问的 Stage2 block 在 kernel evaluation 前被解码
- **当** 某个 query 需要对 packed-magnitude batch block 执行 Stage2 evaluation
- **则** 系统必须将该 block 的 packed magnitude 解码到 scratch
- **并且** 基于 decoded scratch 计算得到的 Stage2 score 必须在浮点误差内匹配 byte-magnitude reference

### Requirement: Stage2 magnitude decode SHALL provide SIMD-accelerated unpack paths
packed Stage2 magnitude decode path MUST 为常见 bit width 提供 SIMD-accelerated unpack implementation，并提供标量参考兜底以保证正确性和可移植性。

#### Scenario: Bits4 decode 使用 nibble-level SIMD unpack
- **当** 在支持 SIMD 的目标平台上解码 `bits=4` packed Stage2 block
- **则** decoder 必须使用 vectorized nibble unpack path 或等价并行实现
- **并且** decoded magnitudes 必须与 scalar unpack output 完全一致

#### Scenario: Bits2 decode 使用 packed-field SIMD unpack
- **当** 在支持 SIMD 的目标平台上解码 `bits=2` packed Stage2 block
- **则** decoder 必须使用 vectorized two-bit unpack path 或等价并行实现
- **并且** decoded magnitudes 必须与 scalar unpack output 完全一致

#### Scenario: Bits3 packed decode 不在本变更中启用
- **当** query path 遇到本变更新格式下的 `bits=3` packed Stage2 block
- **则** 系统必须返回明确的不支持错误或拒绝打开该索引
- **并且** 系统不得使用未经验证的 `bits=3` packed decode path

### Requirement: Stage2 decode overhead SHALL be measurable in benchmarks
基准测试输出 MUST 暴露足够的 timing 或 aggregate counters，用于将 packed Stage2 decode overhead 与 Stage2 scoring、raw-vector fetch 或 payload fetch 分开评估。

#### Scenario: bench_e2e 报告 packed Stage2 decode work
- **当** `bench_e2e` 运行 packed-magnitude Stage2 index
- **则** results metadata 必须报告 packed Stage2 decode 是否启用
- **并且** 它必须报告 aggregate decode timing 或 decode block count，以估计 decode overhead
