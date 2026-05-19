## 背景

已完成的 `nonpow2-padded-hadamard-rotation` 变更已经证明：对于 MSMARCO `768d`，padded-Hadamard 是一个很强的 latency 候选方案，在 recall 基本不变的情况下，查询延迟显著下降。剩余问题在于 footprint。调查显示，索引增大的原因不只是 `.clu`；当前 build 路径还会把 `data.dat` 按 `effective_dim` 写出，因此 raw vector plane 会从 `768` 扩展到 `1024`，几乎翻倍。

现有架构已经将以下内容分离开来：

- `.clu` 中的压缩搜索 / code 数据
- `data.dat` 中的 raw vectors
- 通过地址读从 `data.dat` 做 query-time exact rerank

这使得“存储拆分修复”是可行的。这个变更不需要修改 padded-Hadamard 的 probing 语义，只需要把 raw vector plane 恢复到 `logical_dim`，同时保留 `effective_dim` 给 code 生成和 query prepare 使用即可。

索引构建时还会复用现有 centroids，而不是重新训练聚类。这样可以把本次变更严格限制在 raw storage contract 上，避免把 footprint 修复和聚类质量变化混在一起。

## 目标 / 非目标

**目标：**
- 保持 padded-Hadamard 的查询行为、recall 语义和 `.clu` code 布局不变。
- 即使索引编码 / 搜索使用 `effective_dim`，`data.dat` 也只用 `logical_dim` 存储 raw vectors。
- 确保 exact rerank 和任何其他 raw-vector 读取者都能正确消费 logical-dimension vectors。
- 保持 benchmark / 结果报告清晰，能够把 raw-plane footprint 的降低与 code-plane footprint 分开展示。

**非目标：**
- 不重新设计 `.clu` 的磁盘布局。
- 不把 `effective_dim` 的 code 语义改回 `logical_dim`。
- 不重新训练或重新选择 centroids。
- 不修改 Stage1 / Stage2 kernel、CRC 逻辑或 resident query 调度。
- 不在这个变更里实现更激进的 disk-compact / resident-expand `.clu` 重设计。

## 决策

### 决策：将 raw-vector 存储维度与 code/query 维度拆分
build 路径将继续对以下部分使用 `effective_dim`：
- centroid rotation
- RaBitQ / ExRaBitQ encoding
- `.clu` 生成
- padded-Hadamard 的 query prepare

但它将对以下部分使用 `logical_dim`：
- `data.dat` 的 vector payload 存储
- exact rerank 使用的 raw-vector 读取

这样可以保留 latency 收益，同时消除 footprint 增长中最大、且最容易避免的来源。

**备选方案：** 继续写 padded raw vectors
拒绝原因：零 padding 的尾部对 exact rerank 不提供额外信息，但会在 MSMARCO 规模上让 raw vector plane 几乎翻倍。

### 决策：将 `data.dat` 视为 logical-space 的权威存储
`data.dat` 仍然是原始数据集空间中 exact raw vectors 的来源。查询时 exact rerank 因此会在 `logical_dim` 上比较 query 和 candidate vectors，而不是在 padded raw-storage space 上比较。

**备选方案：** 存 `logical_dim` 并在每次 exact read 时重建 padded raw vectors
拒绝原因：exact rerank 并不需要 padded tail 才能保证距离正确性，因此每次重建都是不必要的开销。

### 决策：保持索引元数据显式暴露两个维度
即使 raw storage 返回到 `logical_dim`，索引仍必须继续暴露：
- `logical_dim`
- `effective_dim`
- `padding_mode`
- `rotation_mode`

这样可以避免存储体积的改进掩盖 `.clu` 和 query prepare 仍然工作在 padded space 这一事实。

**备选方案：** 在 raw-plane 修复后隐藏这个拆分
拒绝原因：这样会让 benchmark 的解释变得模糊，也会让 reopen / debug 更复杂。

### 决策：构建时复用现有 centroids
索引构建阶段将继续使用现有的 centroids 工件，不重新训练聚类，也不改 nlist / cluster assignment 语义。本次变更只调整 raw storage contract 和与 padded-Hadamard 相关的读写维度拆分。

**备选方案：** 在这次变更中顺手重跑聚类
拒绝原因：这样会把 raw storage 修复和聚类质量变化耦合在一起，导致 benchmark 对比失真。

### 决策：benchmark 输出必须分别暴露 raw-plane 与 code-plane footprint
这个变更的动机就是 footprint 回收，因此 benchmark / export 输出必须让我们看到：
- 总 index 字节数
- `data.dat` 字节数
- `.clu` 字节数
- 其他工件字节数

成功标准不仅是总大小下降；更重要的是 raw plane 回到接近 baseline 的水平，同时 padded-Hadamard 的 latency 收益仍然保留。

**备选方案：** 只依赖总字节数
拒绝原因：这样无法判断改进来自 raw plane，还是来自其他布局变化。

## 风险 / 权衡

- [读写维度拆分可能引入微妙不一致] → 在 writer / reader 接口以及 benchmark 输出中显式保留 `logical_dim` 和 `effective_dim`；写入或读取 `data.dat` 时避免隐式复用 `effective_dim`。
- [exact rerank 路径可能误依赖 padded raw vectors] → 审计 rerank / data-file 读取路径，并将 logical-dimension 读取作为 raw-vector storage 的唯一支持契约。
- [footprint 改善了，但 query code 仍然很大] → 将其视为本次变更的显式权衡；`.clu` 压缩留给后续设计。
- [benchmark 解释可能变得混乱] → 分开导出 raw-plane 和 code-plane 的大小，并保持比较记录稳定。
- [复用现有 centroids 可能掩盖聚类不一致问题] → 将 centroids 视为固定输入，在同一聚类结果下只比较存储契约变化。

## 迁移计划

1. 更新 build 路径，使 `data.dat` 只写 `logical_dim` vectors，而 `.clu` 继续按 `effective_dim` 编码。
2. 确认索引构建继续使用现有 centroids，不重新训练聚类。
3. 更新 data-file 的 open / read 契约，使 padded-Hadamard 索引的 exact rerank 读取 `logical_dim` vectors。
4. 重新构建 padded-Hadamard 的 MSMARCO operating point，并比较：
   - recall
   - query latency
   - `data.dat` 字节数
   - 总 index 字节数
5. 在新存储契约验证完成之前，保留之前的 padded-Hadamard full-space build 作为参考点。

## 开放问题

- benchmark 是否应该显式导出 `raw_vector_storage_dim` 字段，还是 `logical_dim` 已经足够？
- 是否存在任何非 rerank 的 `data.dat` 消费者仍然假设 `effective_dim`？
- 一旦 `data.dat` 修复完成，剩余的 `.clu` 增长是否可以接受，还是应该立刻触发一个后续的 compact-on-disk 设计？
