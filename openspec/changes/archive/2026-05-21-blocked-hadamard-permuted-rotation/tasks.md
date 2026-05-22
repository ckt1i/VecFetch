## 1. 旋转内核

- [x] 1.1 在 `RotationMatrix` 中新增独立的 `blocked_hadamard_permuted` 模式，包含分块元数据、置换元数据与确定性 seed 生成逻辑。
- [x] 1.2 实现 `blocked_hadamard_permuted` 的 fast `Apply` 与 `ApplyInverse`，按“置换 + 分块 FWHT + 分块归一化”实现。
- [x] 1.3 更新 `rotation.bin` 的保存/加载与往返测试，保证 blocked-Hadamard 元数据可重现重开。

## 2. 索引构建与查询集成

- [x] 2.1 增加构建配置与旋转模式选择逻辑，使非幂次维度可在无 padding 下使用 `blocked_hadamard_permuted`。
- [x] 2.2 复用旋转质心生成、重开与一次性查询旋转流程，并补充构建元数据上报。
- [x] 2.3 增加正交性、置换可复现性、构建/重开正确性，以及 blocked 模式下 `PrepareQueryRotatedInto` 语义的单测和集成测试。

## 3. MSMARCO 评测

- [x] 3.1 将 MSMARCO 评测配置接入 `/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings`，使用 `16384` 聚类工件并固定 `nprobe = 256`。
- [x] 3.2 运行并记录 `random_matrix`、`padded_hadamard`、`blocked_hadamard_permuted` 的 `recall@10` 与延迟对比。
- [x] 3.3 导出 blocked-Hadamard 实验摘要，包含索引尺寸指标，以便与 padded-Hadamard 的存储结果对照。
