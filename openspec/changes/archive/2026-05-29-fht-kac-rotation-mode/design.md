## Context

当前仓库已有两条非稠密旋转路线：

- `hadamard_padded`：把 `logical_dim` pad 到下一次幂后做完整 Hadamard；
- `blocked_hadamard_permuted`：先做固定置换，再分块做 Hadamard。

前者的优点是混合质量强，但会把 `768 -> 1024`，直接放大编码维度、旋转质心宽度和 `.clu` 的存储宽度。后者保留了 `logical_dim == effective_dim`，但由于旋转矩阵仍是块对角结构，跨块混合不充分。

本次要引入的 `FhtKacRotator` 来自仓库内的 `third_party/RaBitQ-Library`。其实现合同是：

- `padded_dim = round_up_to_multiple(dim, 64)`
- 若 `dim` 不是 2 的幂，则取 `trunc_dim = floor_power_of_2(dim)`
- 重复 4 轮：随机符号翻转 -> 在前/后 `trunc_dim` 窗口交替做 FFHT -> 对两半做 Kac/Givens mixing

这一路线的核心价值不是单次旋转更便宜，而是比当前 blocked 路线更接近随机正交混合，同时避免 `next_power_of_two` 的宽度膨胀。

## Goals / Non-Goals

**Goals:**
- 为非 `2^k` 维度新增 `fht_kac_rotator` 模式，并完整接入 build / reopen / query fast path。
- 保持该模式的有效维度合同为“只 pad 到 64 倍数”，对 `768` 维场景不引入新的维度膨胀。
- 让该模式继续走现有 `query-once + rotated_centroids` 路径，而不是退回每 cluster 重新旋转。
- 给出一个固定参数单点对照实验，判断它在相同工作点下是否优于 `blocked_hadamard_permuted` 与 `hadamard_padded`。

**Non-Goals:**
- 不把本次变更扩展成自动参数搜索、Pareto sweep 或新的论文级实验矩阵。
- 不引入新的 `.clu` 布局或新的量化存储格式。
- 不在本次变更中重构 CRC、Stage2 kernel 或 rerank 逻辑。
- 不要求与 `third_party/RaBitQ-Library` 的 rotator 二进制格式直接兼容。

## Decisions

### Decision: 将 `fht_kac_rotator` 作为独立 rotation kind，而不是覆盖 blocked 模式

它与 `blocked_hadamard_permuted` 的核心区别不只是分块策略，而是“交替 FFHT + Kac mixing”的旋转结构。因此它应当作为独立的 rotation kind 出现在 `RotationMatrix` 与元数据中。

备选方案：在 `blocked_hadamard_permuted` 下新增一个“混合增强”布尔开关。
否决原因：会混淆两种不同的旋转合同，也不利于 reopen 和实验解释。

### Decision: 复用 `RotationMatrix` 抽象，但允许其保存模式专属元数据

现有 builder、encoder、estimator、index reopen 都围绕 `RotationMatrix` 工作。最小改动方式是在该类中新增 `FhtKac` 类型及其元数据：

- `rotation_kind`
- `padded_dim`
- `trunc_dim`
- `num_rounds`
- `sign_sequences`

其中 `sign_sequences` 为 4 组按 `padded_dim` 对齐的随机符号位序列。对于 FhtKac 模式，不再要求保存稠密矩阵字段。

备选方案：新增 `FhtKacRotationMatrix`。
否决原因：接口面会扩散到太多调用者，不值得。

### Decision: FhtKac 的 hot path 只实现 `Apply`，不在首版要求稠密逆矩阵

当前在线路径主要依赖：

- 构建时对 residual / centroid 施加正向旋转
- 查询时对 query 施加正向旋转

并不要求在热路径中频繁调用稠密 `ApplyInverse`。FhtKac 本身是正交变换，但显式逆可以通过“反向轮次”实现。首版可以：

- `Apply` 走 FhtKac fast path
- `ApplyInverse` 用可验证的逆序 round 实现
- 若测试只覆盖正向使用场景，也必须保留逆向一致性校验

备选方案：只支持正向、逆向未定义。
否决原因：会破坏 `RotationMatrix` 现有合同。

### Decision: 对 `768` 维场景保持 `effective_dim == 768`

RaBitQ-Library 的 `FhtKacRotator` 只 pad 到 64 的倍数。对 `768` 维来说，本身已经满足要求，因此：

- `logical_dim = 768`
- `effective_dim = 768`
- `trunc_dim = 512`

这样可以直接避免 `hadamard_padded` 那种 `768 -> 1024` 的宽度放大。

备选方案：统一 pad 到 `max(64-multiple, next_pow2)`。
否决原因：会丢掉 FhtKac 模式最重要的存储优势。

### Decision: 继续复用 `rotated_centroids.bin` 与 `PrepareQueryRotatedInto`

尽管 FhtKac 不是单次标准 Hadamard，它依然是确定的线性正交变换。因此仍可保留：

- 构建时预旋转 centroid
- 查询时 query 只旋转一次
- probe 时做 `rotated_q - rotated_centroid`

这点非常关键，否则 query 端会在每 cluster 上重复做 4 轮旋转，端到端收益会被吃掉。

备选方案：视为通用随机旋转，仅走 `PrepareQueryInto`。
否决原因：会把查询热路径成本抬高到错误量级。

### Decision: 固定参数实验只做一个单点合同

本次只补一个固定参数对照实验，工作点固定为：

- 数据：`/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings`
- `nlist = 16384`
- `nprobe = 256`
- `bits = 4`
- `resident/full_preload`
- `early_stop = 0`

对照只包含：

- `hadamard_padded`
- `blocked_hadamard_permuted`
- `fht_kac_rotator`

备选方案：同时加入 sweep。
否决原因：这会放大实验范围，与用户当前要求不符。

## Risks / Trade-offs

- [FhtKac 单次旋转计算量高于 blocked Hadamard] -> 缓解：继续复用 query-once 路径，把成本限制在“每 query 一次 + build 时一次”。
- [实现比 blocked 更复杂，尤其是逆向与序列化] -> 缓解：首版把元数据结构限制为固定 4 轮与固定角度，不做可配置泛化。
- [理论混合更强，但固定参数单点收益可能有限] -> 缓解：实验合同明确只验证单点，不提前承诺 sweep 结论。
- [和第三方库数值细节不完全一致] -> 缓解：以算法合同兼容为目标，不要求 bitwise 对齐。

## Migration Plan

1. 在 `RotationMatrix` 中新增 `fht_kac_rotator` 类型与模式专属元数据。
2. 实现 FhtKac fast `Apply` / `ApplyInverse`，并补齐往返与正交性测试。
3. 在 builder 选择逻辑中新增 `use_fht_kac_rotator` 开关，并将其接入 `rotation.bin` / `rotated_centroids.bin` / metadata。
4. 在 `IvfIndex::Open` 与查询路径中识别新模式，继续复用 rotated-centroid 快路径。
5. 运行一个固定参数 MSMARCO 对照实验，产出延迟、recall 与尺寸对比。

## Open Questions

- 是否需要把 `num_rounds` 和 mixing angle 暴露为构建参数，还是首版固定为 `4` 轮与 `pi/4`？
- `rotation.bin` 对 FhtKac 是否继续保留稠密矩阵字段用于调试，还是只保存元数据？
- 如果固定参数单点收益不明显，后续是否直接扩展为 “same-recall lower-nprobe” 验证，而不是继续在单点上纠缠？
