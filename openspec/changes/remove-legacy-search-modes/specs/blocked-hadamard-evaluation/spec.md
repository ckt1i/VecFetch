## REMOVED Requirements

### Requirement: MSMARCO blocked Hadamard 评测合同应可复现
**Reason**: Blocked Hadamard is no longer a formal experiment target.
**Migration**: Validate MSMARCO with the FHT-Kac mainline index and current benchmark protocol.

### Requirement: blocked Hadamard 必须与现有基线对齐对比
**Reason**: The project no longer maintains blocked/padded/random rotation comparison as a required benchmark matrix.
**Migration**: Benchmark reports SHALL focus on FHT-Kac mainline performance for non-power-of-two dimensions.

### Requirement: 评测报告需同步输出存储结果
**Reason**: Storage comparison against blocked Hadamard is no longer a required output.
**Migration**: Continue reporting index storage metrics for the mainline FHT-Kac operating point.
