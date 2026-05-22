## ADDED Requirements

### Requirement: MSMARCO blocked Hadamard 评测合同应可复现
系统 SHALL 明确定义 `blocked_hadamard_permuted` 在 MSMARCO passage 上的可复现实验合同。合同 SHALL 使用 `/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings` 作为 embeddings 根目录，SHALL 使用该目录下 `16384` 聚类工件，并 SHALL 在 `nprobe = 256` 下评测。

#### Scenario: 固定输入工件
- **WHEN** 执行 blocked-Hadamard 评测
- **THEN** 使用声明的 embeddings 根目录下的 MSMARCO base/query embeddings 与 `16384` 聚类文件

#### Scenario: 固定搜索工作点
- **WHEN** 输出 blocked-Hadamard 对比的延迟与召回
- **THEN** 报告中必须包含 `nlist = 16384` 与 `nprobe = 256`

### Requirement: blocked Hadamard 必须与现有基线对齐对比
评测 SHALL 在相同数据与相同工作点下，比较 `blocked_hadamard_permuted` 与现有 `random_matrix`、`padded_hadamard`。报告 SHALL 包含 `latency` 与 `recall@10`。

#### Scenario: 覆盖基线集
- **WHEN** blocked-Hadamard 评测完成
- **THEN** 结果集中必须同时出现 `random_matrix`、`padded_hadamard`、`blocked_hadamard_permuted`

#### Scenario: 保留用户给定参照点
- **WHEN** 生成实验计划或结果摘要
- **THEN** 保留既有参照值：`random_matrix` 约 `13 ms`，`padded_hadamard` 约 `7 ms` 且 `recall@10 ~= 0.96`

### Requirement: 评测报告需同步输出存储结果
评测 SHALL 报告 blocked-Hadamard 是否避免了 padding 带来的宽度膨胀，通过输出构建产物尺寸让读者能分辨召回/延迟收益与存储变化。

#### Scenario: 存储对比可见
- **WHEN** 导出 blocked-Hadamard 基准结果
- **THEN** 包含足够粒度的索引尺寸指标，支持与 padded-Hadamard 建索引结果进行对比
