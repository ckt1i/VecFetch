## Context

当前 RaBitQ 里可用的旋转方式主要有两类：

- `random_matrix`：任意维度都可用，但应用代价为 `O(D^2)`；
- full Hadamard：可走 FWHT，但仅在有效维度是 2 的幂时才可用。

MSMARCO passage 的 `logical_dim=768`，`nonpow2-padded-hadamard-rotation` 已证明 `768 -> 1024` 的 padding 在 `nprobe=256` 下可把延迟从约 `13 ms` 降到约 `7 ms`，`recall@10` 约 `0.96`。但该方案把编码/检索维度扩到 `1024`，导致 `.clu`的文件大小暴增到11GB，旋转质心存储和线性扫描成本上升。

本提案的替代路线是 `blocked_hadamard_permuted`：保持逻辑维和有效维都为 `768`，先做一次固定种子的维度置换，再按 `512 + 256` 这类 2 的幂块切分，每块独立做 Hadamard。这样保持全链路仍为 FWHT 热路径，同时避免 padded 宽度扩张。

## Goals / Non-Goals

**Goals:**
- 为非 2 的幂维度新增确定性的旋转模式，热路径只走分块 FWHT，不在 hot path 回退到密集 `O(D^2)`。
- 保持 blocked-Hadamard 下 `logical_dim == effective_dim`，使编码宽度、`.clu` 和旋转质心宽度都在原始维度上。
- 复用现有“先旋转质心再一次性旋转查询”的路径，保证查询准备语义与当前 Hadamard 快路径一致。
- 给出明确可落地的 MSMARCO 评测方案：复用现有 embedding 目录、`16384` 聚类工件、`nprobe=256`。

**Non-Goals:**
- 除去除 padded 宽度需求之外，不重构 `.clu` 布局。
- 不引入学习型投影、稠密混合矩阵或混合旋转模型。
- 首版不支持用户自定义分块策略，分块仅按维度分解确定性生成。
- 除了消化新旋转模式外，不改 Stage2 kernel、CRC 策略和 rerank 语义。

## Decisions

### Decision: 将 `blocked_hadamard_permuted` 作为独立旋转模式引入

新模式应体现在构建元数据与持久化工件中，而不是复用已有 `hadamard` 标志推断分块；否则会导致 reopen 与对比实验解释不清。

备选方案：复用 `hadamard` 并按维度推断块结构。
否决原因：`768` 不能表示为单一完整 Hadamard，且隐式推断使持久化索引难以解释。

### Decision: 分块大小采用确定性的二次幂分解

首版采用 `dim` 的贪心二进制分解、从大到小排序。`768` 的分块为 `[512, 256]`。方案简单、确定且每块都支持 FWHT。

备选方案：对大量分块组合做搜索选择。
否决原因：会在基础能力和性能验证前引入过多实验变量。

### Decision: 分块前先做一次固定随机种子的置换

先应用固定置换 `perm[dim]`，再做分块 Hadamard+符号对角。这样可打散原始坐标顺序，避免直接用 `0..511 | 512..767` 的连续切分带来的维序结构偏差。

备选方案：不做置换直接连续切分 `[512, 256]`。
否决原因：实现简单但混合能力更弱，更依赖原始 embedding 的维度顺序。

### Decision: 扩展 `RotationMatrix` 元数据和 fast path，而不是新增类型

`RotationMatrix` 已是 builder、encoder、estimator、重开链路的共享契约。首版最小改动方式是扩展该类，增加:

- 旋转类型标识
- 块大小
- permutation 与可逆映射
- 按分块/置换顺序的符号向量

这样可以继续沿用单一旋转抽象，不改动更多上层接口。

备选方案：新增 `BlockedRotationMatrix` 类型。
否决原因：会迫使 encoder、estimator、builder、重开逻辑都改接口，收益不足。

### Decision: 复用现有旋转查询与旋转质心快路径

blocked-Hadamard 仍是正交线性变换，应保持以下流程：

- 构建时预计算 `rotated_centroids.bin`
- 搜索时查询只旋转一次
- 调用 `PrepareQueryRotatedInto(rotated_q, rotated_centroid, ...)`

这样可确保 cluster probe 热路径与现有 Hadamard 优化对齐，不引入每簇重复旋转。

备选方案：将其当通用旋转走 `PrepareQueryInto`。
否决原因：会放弃主要的查询时加速收益。

### Decision: 保留 `rotation.bin` 稠密矩阵存储用于兼容，但 hot path 以分块元数据为准

现有文件格式与部分测试仍依赖 `rotation.bin` 中的稠密矩阵字段。首版可以继续存 `data_`，并额外序列化分块元数据，`Apply/ApplyInverse` 在 `rotation_mode == blocked_hadamard_permuted` 时走分块 fast path。

备选方案：blocked 模式不再保存稠密矩阵。
否决原因：首版会引入较高迁移和兼容风险，收益不成比例。

### Decision: 固定参考点和工作点进行评测

评测合同应明确：

- embeddings 根目录：`/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings`
- centroids：`msmarco_passage_centroid_16384.fvecs`
- assignments：`msmarco_passage_cluster_id_16384.ivecs`
- `nlist=16384`
- `nprobe=256`

对比至少包含：

- `random_matrix`
- `padded_hadamard`
- `blocked_hadamard_permuted`

这样可在可复现实验点上对齐用户已给定的延迟/召回参照。

备选方案：先实现后再评测。
否决原因：本次需求本身就是为性能与召回取舍服务。

## Risks / Trade-offs

- [分块混合强度弱于完整 padded-Hadamard] -> 缓解：固定置换后再分块，并要求与两个基线做 recall 对比。
- [旋转持久化复杂度上升] -> 缓解：明确 `rotation.bin` 元数据版本并补齐重开/重序列化测试。
- [`used_hadamard` 名称与语义可能不完全一致] -> 缓解：短期保持兼容行为，以“是否启用 fast pre-rotated-centroid 流程”作为运行条件，后续再逐步清理命名。
- [稠密矩阵与分块元数据并存导致 `rotation.bin` 变大] -> 缓解：首版接受此成本，因为该文件不是主导 footprint，兼容性优先。
- [基准噪声掩盖小幅收益] -> 缓解：固定工作点重复多次并报中位数延迟与 recall。

## Migration Plan

1. 在 `RotationMatrix` 中实现 blocked 生成、`Apply`/`ApplyInverse` 分块 fast path 与持久化。
2. 扩展 builder 配置与选择逻辑，支持非幂维度在无 padding 下开启 `blocked_hadamard_permuted`。
3. 复用已有旋转质心生成/加载与一次性查询旋转流程。
4. 增加正交性、持久化可复现、build/reopen、旋转查询语义的单测与集成测试。
5. 用固定 `random_matrix` 与 `padded_hadamard` 基线做 MSMARCO 对比。
6. 该模式保持可选，回退策略为继续使用现有 `random_matrix` 或 `padded_hadamard` 构建。

## Open Questions

- `rotation.bin` 先采用显式 format version，还是在现有尾部元数据中扩展？
- 构建元数据里只记录分块列表和 seed 是否足够，还是需要再记录可读的 permutation 合同说明？
- 首轮实验是否必须输出 `cluster.clu`、`rotated_centroids.bin`、总索引字节等尺寸拆分，而不只输出延迟与 recall？
