## ADDED Requirements

### Requirement: DiskANN evaluation SHALL use corrected overlap recall
DiskANN Pareto 评测流程 SHALL 将正式上报的 `recall@10` 定义为相对于配置好的 ground-truth top-k 结果的平均 top-k overlap。

#### Scenario: Corrected recall is computed for a query batch
- **WHEN** DiskANN 为一批 query 产出 top-k ID，且 ground-truth ID 可用
- **THEN** 流程 SHALL 将每个 query 的 recall 计算为 `|predicted_topk ∩ ground_truth_topk| / topk`
- **AND** 导出的 batch recall SHALL 等于各 query recall 的平均值

#### Scenario: Old hit-rate recall is not used for Pareto selection
- **WHEN** 流程在选择 Pareto 点或目标 recall operating point
- **THEN** 它 SHALL 使用修正后的 overlap recall
- **AND** 它 MUST NOT 再使用“query 级任意命中即算正确”的旧 recall 作为选择指标

### Requirement: DiskANN search SHALL be measured through the C++ CLI path
DiskANN Pareto 评测流程 SHALL 使用 DiskANN 的 C++ disk-search 可执行路径作为正式测量结果的唯一权威搜索路径。

#### Scenario: C++ search results are parsed
- **WHEN** 某个 DiskANN search sweep 点开始运行
- **THEN** 流程 SHALL 调用配置好的 C++ DiskANN 搜索可执行文件
- **AND** 它 SHALL 将输出的 top-k 结果 ID 解析进评测结果 schema

#### Scenario: Python binding output is excluded from authoritative results
- **WHEN** 同一数据集同时存在 Python binding 产生的 DiskANN 输出
- **THEN** 流程 SHALL NOT 将该输出写入正式 Pareto CSV
- **AND** 任何仅用于诊断的 Python binding 结果 SHALL 被明确标记为 non-authoritative

### Requirement: COCO100k sweep SHALL run before MSMARCO sweep
执行计划 SHALL 先完成 COCO100k，再进入 MSMARCO 的 DiskANN 建索引与评测。

#### Scenario: COCO sweep uses existing indexes first
- **WHEN** COCO100k DiskANN Pareto 运行开始
- **THEN** 它 SHALL 先在现有已验证的 COCO100k DiskANN 索引上扫描搜索期参数
- **AND** 只有当现有索引无法覆盖目标 recall 区间时，它才 SHALL 安排新的 COCO100k 索引构建

#### Scenario: MSMARCO starts after COCO first-pass results
- **WHEN** 第一轮 COCO100k sweep 与有效性校验完成
- **THEN** 流程 SHALL 进入 MSMARCO DiskANN 索引构建阶段
- **AND** 它 SHALL NOT 要求先完成最终 COCO 图表整理后才能开始 MSMARCO

### Requirement: MSMARCO DiskANN indexes SHALL be reproducibly constructed
流程 SHALL 基于已配置的 formal baseline 资产构建 MSMARCO DiskANN disk index，并在每个索引旁写出可复现元数据。

#### Scenario: MSMARCO index build writes a manifest
- **WHEN** 某个 MSMARCO DiskANN 索引构建完成
- **THEN** 索引目录 SHALL 包含一个 manifest，记录 dataset 名称、向量维度、metric、base vectors 来源、query vectors 来源、ground-truth 路径、DiskANN 可执行路径、构建参数和输出前缀

#### Scenario: Companion files are validated
- **WHEN** 某个已构建或已有 DiskANN 索引被选中用于 C++ 搜索
- **THEN** 流程 SHALL 在搜索前校验必需的 companion 文件是否存在，或以确定性方式生成这些文件

### Requirement: DiskANN plus FlatStor timing SHALL be exported
流程 SHALL 将 DiskANN 搜索延迟与 FlatStor payload 读取延迟分开测量，并导出二者之和作为可对比的 DiskANN 端到端延迟。

#### Scenario: Payload timing is measured for DiskANN results
- **WHEN** DiskANN 搜索为某个 query 产出 top-k ID
- **THEN** 流程 SHALL 通过 FlatStor 读取对应 payload
- **AND** 它 SHALL 为该 query 或该 batch 记录 payload-read latency

#### Scenario: Combined latency is exported
- **WHEN** 某个 sweep 点运行完成
- **THEN** 输出 SHALL 同时包含 DiskANN search latency、FlatStor payload-read latency 和 combined latency
- **AND** combined latency SHALL 等于测量协议下的 `search(DiskANN) + read_payload(FlatStor)`

### Requirement: Pareto outputs SHALL preserve reproducibility and validity fields
流程 SHALL 导出原始 sweep 行和筛选后的 operating point，并保留足够的元数据来重建结果和识别无效运行。

#### Scenario: Sweep row includes parameter identity
- **WHEN** 某个有效 sweep 点被写出
- **THEN** 该行 SHALL 包含 dataset、index identity、top-k、query count、`R`、build `L`、search `L`、beam width、PQ settings（如适用）、cache setting、recall、latency 字段、duplicate rate 和 result status

#### Scenario: Invalid point is marked
- **WHEN** 某个 sweep 点存在重复 ID、top-k 输出不完整、query 与 ground-truth 不匹配、payload 缺失、超时或解析失败
- **THEN** 流程 SHALL 将该点标记为 invalid，并记录原因
- **AND** 它 SHALL 将该点排除在 Pareto frontier 和 target-threshold selection 之外

#### Scenario: Target recall summary is generated
- **WHEN** 某个数据集的 sweep 中包含有效点
- **THEN** 流程 SHALL 为 `0.85`、`0.90`、`0.95`、`0.98` 和 `0.995` 等目标 `recall@10` 生成汇总
- **AND** 任何无法达到的目标 SHALL 被标记为 unreached，而不是被静默省略
