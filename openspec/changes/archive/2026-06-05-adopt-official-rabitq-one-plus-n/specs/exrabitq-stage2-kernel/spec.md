## ADDED Requirements

### Requirement: Stage2 kernel SHALL implement official 1+n ExData scoring
Stage2 query scoring SHALL support official `1+n` ExData. 对于 `ex_bits > 0` 的候选，kernel MUST combine Stage1 intermediate `ip_x0_qr` with ExData inner product and official factors, instead of consuming a separate Stage2 sign stream.

#### Scenario: Official Stage2 combines Stage1 and ExData terms
- **WHEN** 一个候选进入 official Stage2 evaluation
- **THEN** Stage2 必须使用该候选的 `ip_x0_qr`
- **AND** 必须计算 sign-folded ExData code 与 query 的 inner product
- **AND** 必须按 official formula 组合 `2^ex_bits * ip_x0_qr + ip_ex + factor terms`

#### Scenario: Official Stage2 does not read legacy sign blocks
- **WHEN** official Stage2 kernel 处理一个候选
- **THEN** 它不得要求 `ex_sign_packed`
- **AND** 它不得从 Stage1 FastScan block 临时解码 sign 来构造 signed-magnitude code

### Requirement: Stage1 SHALL expose reusable ip_x0_qr for Stage2 candidates
Stage1 candidate generation SHALL expose or persist enough intermediate data for official Stage2 to obtain `ip_x0_qr` without rescanning Stage1 BinData for each candidate.

#### Scenario: Candidate metadata carries Stage1 intermediate value
- **WHEN** Stage1 将一个 uncertain candidate 交给 Stage2
- **THEN** candidate metadata 必须包含 `ip_x0_qr` 或可无损转换为 `ip_x0_qr` 的 raw accumulator
- **AND** Stage2 必须使用该值进行 official score composition

#### Scenario: Reference path validates ip_x0_qr
- **WHEN** diagnostic 或 unit test 对一个 candidate 运行 official scalar reference
- **THEN** scalar reference 推导的 `ip_x0_qr` 必须与 Stage1 传递值在容忍误差内一致

### Requirement: Official ExData kernels SHALL cover target bit widths
official ExData kernel SHALL support the target experiment bit widths, including `ex_bits=0`, `ex_bits=2`, `ex_bits=3`, and `ex_bits=4`.

#### Scenario: Ex bits zero uses Stage1 only
- **WHEN** `ex_bits=0`
- **THEN** query 必须跳过 Stage2 ExData dot
- **AND** final approximate score 必须来自 Stage1 estimator

#### Scenario: Ex bits three has SIMD or measured fallback support
- **WHEN** `ex_bits=3`
- **THEN** query 必须能正确计算 official ExData score
- **AND** implementation 必须提供 SIMD path 或带独立 timing metadata 的 decode-to-scratch fallback path

#### Scenario: SIMD kernel matches scalar reference
- **WHEN** SIMD official ExData kernel 与 scalar reference 处理相同 query 和 packed ExData
- **THEN** 两者输出必须在指定浮点误差内一致
- **AND** 测试必须覆盖 tail dimensions、partial lanes and valid lane masks

### Requirement: Stage2 classification SHALL use official score semantics
SafeIn/SafeOut Stage2 classification SHALL consume official Stage2 estimates when the index format is official `1+n`, and SHALL continue to consume legacy signed-magnitude estimates for legacy indexes.

#### Scenario: Official index uses official Stage2 margin
- **WHEN** runtime 查询 official `1+n` index
- **THEN** Stage2 SafeIn/SafeOut classification 必须基于 official Stage2 estimate
- **AND** margin divisor 必须使用 `total_bits` semantics

#### Scenario: Legacy index keeps legacy Stage2 classification
- **WHEN** runtime 查询 legacy signed-magnitude index
- **THEN** Stage2 SafeIn/SafeOut classification 必须保持 legacy behavior
- **AND** 不得因 official support 改变旧索引 recall behavior
