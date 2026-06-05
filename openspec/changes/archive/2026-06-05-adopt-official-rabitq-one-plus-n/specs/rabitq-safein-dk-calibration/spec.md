## ADDED Requirements

### Requirement: SafeIn calibration SHALL use official total_bits semantics
当索引使用 official `1+n` RaBitQ format 时，SafeIn/SafeOut calibration SHALL use `total_bits` for error-bound semantics and SHALL use official Stage2 estimator scores for Stage2 calibration domains.

#### Scenario: Official calibration records total and ex bits
- **WHEN** calibration 为 official `1+n` index 生成 threshold metadata
- **THEN** metadata 必须包含 `rabitq_total_bits`
- **AND** metadata 必须包含 `rabitq_ex_bits`
- **AND** metadata 必须包含 `rabitq_estimator_mode=official_1_plus_n`

#### Scenario: Official Stage2 scores drive RabitQ-space calibration
- **WHEN** SafeIn RabitQ-space `d_k` 或 candidate-level CRC threshold 对 official index 校准
- **THEN** calibration 必须使用 official Stage2 estimate
- **AND** 不得使用 legacy signed-magnitude Stage2 score 代替

### Requirement: Calibration SHALL preserve legacy fallback behavior
calibration runtime SHALL keep legacy signed-magnitude calibration behavior for old indexes and SHALL NOT require rebuilding old indexes solely because official `1+n` support exists.

#### Scenario: Legacy index keeps legacy calibration semantics
- **WHEN** calibration 或 query runtime 打开 legacy signed-magnitude index
- **THEN** 它必须使用 legacy bit semantics 和 legacy Stage2 estimator metadata
- **AND** 不得将 legacy threshold 标记为 official `1+n`

#### Scenario: Missing official metadata fails safely
- **WHEN** official `1+n` query mode 被要求但 index 缺少 `total_bits/ex_bits/estimator_mode` metadata
- **THEN** runtime 必须失败或降级到明确标记的 legacy fallback
- **AND** 不得静默使用不明 bit semantics 进行 SafeIn/SafeOut 判定

### Requirement: ConANN epsilon SHALL consume total_bits for official indexes
ConANN epsilon and margin-related formulas SHALL use `total_bits` when running official `1+n` indexes, while Stage2 packing and kernels SHALL use `ex_bits`.

#### Scenario: Official epsilon uses total bits
- **WHEN** runtime 为 official `1+n` index 构造 ConANN/SafeOut 参数
- **THEN** epsilon formula 必须使用 `rabitq_total_bits`
- **AND** 不得错误使用 `rabitq_ex_bits`

#### Scenario: Stage2 kernel uses ex bits
- **WHEN** runtime unpack 或 evaluate official ExData
- **THEN** Stage2 kernel 必须使用 `rabitq_ex_bits` 解释 packed code width
- **AND** 不得错误使用 `rabitq_total_bits` 作为 ExData payload width
