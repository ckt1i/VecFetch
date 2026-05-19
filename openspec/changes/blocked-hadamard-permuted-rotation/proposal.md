## Why

MSMARCO passage 的向量是 `768` 维，不满足当前完整维度 Hadamard 的幂次约束。现有的 `padded_hadamard` 方案（`768 -> 1024`）在延迟上明显优于 `random_matrix`，但会把 `.clu`、`rotated_centroids.bin` 等工件拉到 `1024` 维，导致索引与扫描宽度膨胀。

## What Changes

- 新增面向非 2 的幂维度的非 Padding 旋转模式：`blocked_hadamard_permuted`。
- 实现确定性分块旋转：先对逻辑维度做一次固定的随机种子打乱，再按 2 的幂块分解后分别做 Hadamard。
- 对该模式保持 `logical_dim == effective_dim`，在 MSMARCO 上仍按 `768` 维构建编码、旋转质心和查询准备。
- 扩展旋转持久化、重开与构建元数据，让 blocked-Hadamard 索引可准确重放和识别。
- 复用已有一次性旋转质心快路径，让 blocked-Hadamard 走 `PrepareQueryRotatedInto` + 预计算 `rotated_centroids.bin`。
- 增加 MSMARCO 对照实验方案：使用 `/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings`，`nlist=16384`，`nprobe=256`，对比：
- `random_matrix` 参考：约 `13 ms`
- `padded_hadamard` 参考：约 `7.1 ms`，`recall@10 ~= 0.9615`
- 新方案 `blocked_hadamard_permuted`

## Capabilities

### New Capabilities
- `blocked-hadamard-rotation`：支持非 2 的幂维度下的 `blocked_hadamard_permuted` 旋转（含 permutation、持久化、重开和查询快路径接入），并且不使用 padding。
- `blocked-hadamard-evaluation`：定义 MSMARCO 评测对照合同（与 `random_matrix`、`padded_hadamard` 对比）。

### Modified Capabilities

## Impact

- 受影响代码：`rabitq_rotation`、`ivf_builder`、`ivf_index`、旋转质心生成与加载、构建元数据、benchmark 执行链与相关测试。
- 受影响工件：`rotation.bin`、`rotated_centroids.bin`、`build_metadata.json`、benchmark 输出。
- 预期结果：在不做 `768 -> 1024` 扩展的情况下，尽量保留 padded-Hadamard 的大部分延迟收益。
