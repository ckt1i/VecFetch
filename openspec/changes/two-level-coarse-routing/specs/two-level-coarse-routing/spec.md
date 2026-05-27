## ADDED Requirements

### Requirement: 双层 coarse routing SHALL 作为可选的 coarse select 模式提供
系统 SHALL 为 IVF 查询时的 cluster select 提供一个可选的双层 coarse routing 模式，同时保留现有的 exact 全量 centroid coarse select 路径。

#### Scenario: exact routing 继续可用
- **WHEN** 双层 coarse routing 被关闭
- **THEN** `FindNearestClusters()` SHALL 按现有 exact coarse select 行为对一层 centroids 打分
- **AND** 返回的 cluster IDs SHALL 遵循现有 top-`nprobe` 约定

#### Scenario: 双层 routing 仅在开启且满足条件时使用
- **WHEN** 双层 coarse routing 被开启且 `nlist` 大于配置阈值
- **THEN** 查询路径 SHALL 使用已经缓存且有效的双层 hierarchy
- **AND** 如果 hierarchy 不可用或无效，则 SHALL 回退到 exact routing

### Requirement: 双层 hierarchy SHALL 只构建一次并缓存
系统 SHALL 基于已加载的一层 IVF centroids 构建一个 super-centroid hierarchy，并在多个查询之间复用这份 hierarchy。

#### Scenario: hierarchy 不按 query 重建
- **WHEN** 多个查询在同一个已打开的 index 和相同 routing 配置下运行
- **THEN** 系统 SHALL 复用同一份内存中的双层 hierarchy
- **AND** 它 SHALL NOT 在每个查询上重新对一层 centroids 聚类

#### Scenario: 默认 super-cluster 数量由 nlist 推导
- **WHEN** 没有显式提供 super-cluster 数量 override
- **THEN** hierarchy builder SHALL 使用 `ceil(nlist / 128)` 个 super clusters
- **AND** 它 SHALL 将该结果 clamp 到当前 `nlist` 的合法范围内

### Requirement: 双层查询 routing SHALL 使用受 budget 控制的 child centroid scoring
双层查询路径 SHALL 先通过 super-centroid selection 选出一个有界的 child centroid candidate 集合，再对这些 child centroids 使用真实的一层 centroid score 进行打分。

#### Scenario: candidate budget 从 nprobe 推导
- **WHEN** 双层 routing 在选择 child centroid candidates
- **THEN** 目标 child candidate budget SHALL 默认为 `8 * nprobe`
- **AND** 被探测的 super clusters 数量 SHALL 由这个 budget 和每个 super cluster 的平均 child 数共同决定

#### Scenario: 最终 cluster 顺序由 child centroid score 决定
- **WHEN** 双层 routing 已经选出 child centroid candidates
- **THEN** 最终的 top-`nprobe` cluster IDs SHALL 使用针对原始一层 centroids 计算出的分数来决定
- **AND** super-centroid score SHALL NOT 作为最终一层 centroid 排序分数使用

#### Scenario: child candidates 不足时安全回退
- **WHEN** 双层路径无法收集到至少 `nprobe` 个有效的 child centroid candidates
- **THEN** 系统 SHALL 回退到 exact coarse select
- **AND** 查询结果 SHALL 仍然返回最多 `nprobe` 个有效 cluster IDs

### Requirement: benchmark 输出 SHALL 标识双层 coarse routing 配置和诊断信息
benchmark 输出 SHALL 包含足够的元数据，用于复现和评估双层 coarse routing 的运行结果。

#### Scenario: 导出 routing 配置
- **WHEN** `bench_e2e` 以双层 coarse routing 运行
- **THEN** 输出 SHALL 包含 routing mode、threshold、super-cluster count、budget factor、child candidate budget 和 probed super-cluster count

#### Scenario: 导出 routing 诊断信息
- **WHEN** 双层 coarse routing 被开启
- **THEN** benchmark 输出 SHALL 报告平均 child candidates scored
- **AND** 它 SHALL 报告与 exact routing 对比所需的 coarse timing 字段

#### Scenario: recall 验证使用真实 ground truth
- **WHEN** 双层 coarse routing 的 benchmark 结果被用于得出性能结论
- **THEN** 该运行 SHALL 使用真实 ground-truth recall，而不是 `--skip-gt 1`
- **AND** recall 指标 SHALL 与延迟指标一同报告
