## 为什么要做

已完成的 `nonpow2-padded-hadamard-rotation` 实验已经证明了这个思路在 MSMARCO 上的延迟收益：在 recall 基本不变的前提下，查询延迟有明显下降。当前剩余的问题是存储效率：现有实现会把 `data.dat` 按 `effective_dim`（`1024`）而不是 `logical_dim`（`768`）写盘，因此 raw vector plane 的大小被放大了一倍，最终主导了整体索引 footprint。

## 变更内容

- 保持 padded-Hadamard 的查询语义和 `.clu` code 语义不变，仍然使用 `effective_dim`。
- 构建索引时复用现有 centroids，不重新训练聚类；这次变更只调整 raw vector plane 的存储维度。
- 将 raw vector plane（`data.dat`）改为即使在使用更大 `effective_dim` 的 padded-Hadamard 索引中，也只持久化 `logical_dim` 个浮点数。
- 保留 `logical_dim` / `effective_dim` 元数据，并让读路径显式地从 `logical_dim` 的 raw vectors 中重建 exact-distance 输入，而不是依赖 padded raw storage。
- 扩展 benchmark / 结果报告，使 padded-Hadamard 的对比能够在 raw-vector-plane footprint 与 code-plane footprint 之间做出区分。

## 能力影响

### 新能力
- 无。

### 修改的能力
- `ivf-rabitq-baseline-storage`：raw vector plane 的要求发生变化，padded-Hadamard 索引要在 `logical_dim` 下存储 raw vectors，而压缩后的 cluster code 仍然保留 `effective_dim`。
- `query-pipeline`：即使 raw vectors 是按 `logical_dim` 读回，只要 probe/code 路径仍使用 `effective_dim`，exact-vector reads 和 rerank 行为也必须保持正确。
- `e2e-benchmark`：benchmark 输出仍然需要比较 padded-Hadamard 的 footprint，但现在还必须明确展示 raw-vector 存储契约已经缩小。

## 影响

- 受影响的代码：IVF builder / data-file 写入、data-file 读取、rerank / exact-distance 路径、benchmark footprint 统计。
- 受影响的工件：`data.dat` 布局契约、padded-Hadamard 的 benchmark 结果解释。
- 主要目标：在不牺牲已经在 MSMARCO 上验证过的 latency 收益的前提下，尽量回收 padded-Hadamard 带来的 footprint 增长。
