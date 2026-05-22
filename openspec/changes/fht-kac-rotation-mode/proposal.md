## Why

当前 `blocked_hadamard_permuted` 已经解决了非 `2^k` 维度必须扩到下一次幂的问题，但它本质上仍是块对角旋转，跨块混合能力有限。RaBitQ-Library 使用的 `FhtKacRotator` 只要求维度 pad 到 `64` 的倍数，并通过多轮符号翻转、交替 FFHT 与 Kac/Givens mixing 近似随机正交变换，更有机会在相同 `nprobe` 下提升 recall，或在相同 recall 下允许更激进的 serving 参数。

## What Changes

- 新增一个独立的 `fht_kac_rotator` rotation mode，用于非 `2^k` 维度的 RaBitQ 构建与查询路径。
- 该模式保持“只 pad 到 `64` 的倍数”的合同；对 `768` 维 MSMARCO，`effective_dim` 仍保持为 `768`。
- 在旋转元数据、builder 选择逻辑、index reopen、query-once 旋转与 `rotated_centroids.bin` 生成链路中接入 `fht_kac_rotator`。
- 为 `rotation.bin` 增加足够的持久化元数据，使同一 seed/配置下的 Fht+Kac 旋转可重放。
- 增加一个固定参数对照实验合同，仅比较:
  - `hadamard_padded`
  - `blocked_hadamard_permuted`
  - `fht_kac_rotator`
  在同一 MSMARCO 工作点下的端到端延迟、recall 与索引尺寸。
- 本次变更不引入参数搜索，不扩展为新的大规模实验矩阵。

## Capabilities

### New Capabilities
- `fht-kac-rotation`: 支持使用 FhtKacRotator 风格的近似随机旋转模式进行非 `2^k` 维度的 IVF-RaBitQ 构建、持久化、重开与查询。
- `fht-kac-fixed-evaluation`: 定义一个固定参数的 MSMARCO 对照实验合同，用于与 `hadamard_padded` 和 `blocked_hadamard_permuted` 做单点比较。

### Modified Capabilities

## Impact

- 受影响代码：`rabitq_rotation`、`ivf_builder`、`ivf_index`、query prepare/rotation hot path、benchmark CLI 与结果导出。
- 受影响工件：`rotation.bin`、`rotated_centroids.bin`、`build_metadata.json`、benchmark 结果文件。
- 主要依赖：复用仓库内已有的 `third_party/RaBitQ-Library` 算法描述，不引入新的外部运行时依赖。
