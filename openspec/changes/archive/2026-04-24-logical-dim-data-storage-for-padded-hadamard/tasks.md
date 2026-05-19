## 1. 原始向量存储契约

- [x] 1.1 审计 build 路径，找出所有当前在写 `data.dat` 时重复使用 `effective_dim` 的位置
- [x] 1.2 更新 raw vector 写入路径，使 padded-Hadamard build 在 `data.dat` 中每条记录只写 `logical_dim` 个浮点数
- [x] 1.3 保留 `.clu`、rotated centroids 和 code 生成路径对 `effective_dim` 的使用
- [x] 1.4 确认索引构建继续复用现有 centroids，不重新训练聚类

## 2. 读取路径与 exact rerank

- [x] 2.1 更新 data-file 的 reader / open 契约，使 padded-Hadamard 索引按 `logical_dim` 读取 raw vectors
- [x] 2.2 验证 rerank / exact-distance 代码能够消费 logical-dimension raw vectors，而不依赖 padded raw storage
- [x] 2.3 检查是否存在任何非 rerank 的 `data.dat` 消费者仍假设 `effective_dim`，并将它们对齐到 logical-dimension 契约

## 3. Benchmark 与 footprint 报告

- [x] 3.1 扩展 benchmark / 结果报告，使 raw vector plane 大小与 compressed code plane 大小在 storage split 之后仍然可区分
- [x] 3.2 以 logical-dimension raw storage 重新构建 MSMARCO padded-Hadamard operating point
- [x] 3.3 在 raw-storage 修复后，导出一份稳定的 baseline vs padded-Hadamard 对比记录

## 4. 验证

- [x] 4.1 验证 baseline 的非 padded 索引不受影响
- [x] 4.2 验证 `data.dat` 返回到 `logical_dim` 后，padded-Hadamard 的 recall 和 latency 仍然处于预期范围
- [x] 4.3 验证 `data.dat` footprint 明显下降，而 `.clu` / code-plane 语义保持不变
