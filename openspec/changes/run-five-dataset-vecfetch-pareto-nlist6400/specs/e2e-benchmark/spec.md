## ADDED Requirements

### Requirement: 正式 VecFetch E2E 运行必须使用外部 top-100 ground truth
E2E benchmark 工作流必须支持正式 Full VecFetch 查询运行，并且 `topk=10` 和 `topk=100` 都使用该数据集的外部 `gt_top100.npy` 文件作为 recall 来源。

#### Scenario: top-k 10 使用 top-100 GT
- **WHEN** 调度 `topk=10` 的正式 VecFetch E2E 运行
- **THEN** benchmark 命令必须使用该数据集的 `gt_top100.npy` 作为 `--gt-file`
- **AND** 结果必须报告 `recall_available=true`
- **AND** aggregate 必须将 `recall_at_k` 映射为 `Recall@10`。

#### Scenario: top-k 100 使用 top-100 GT
- **WHEN** 调度 `topk=100` 的正式 VecFetch E2E 运行
- **THEN** benchmark 命令必须使用该数据集的 `gt_top100.npy` 作为 `--gt-file`
- **AND** 结果必须报告 `recall_available=true`
- **AND** aggregate 必须将 `recall_at_k` 映射为 `Recall@100`。

#### Scenario: top-10 GT 运行从正式输出中拒绝
- **WHEN** benchmark 结果引用 `gt_top10.npy`
- **THEN** 该结果可以作为 smoke 或历史运行保留
- **AND** 不得提升到本 change 的正式 Pareto CSV 或 threshold-selected summary。

### Requirement: 正式 VecFetch E2E 命令必须复用 accepted indexes
E2E benchmark 工作流必须通过加载预先存在的索引来运行正式 Full VecFetch 点，而不是在查询测量期间构建索引。

#### Scenario: 必须提供已有索引路径
- **WHEN** 生成正式 Full VecFetch E2E 命令
- **THEN** 命令必须包含 `--index-dir`，并指向该数据集 accepted VecFetch index path
- **AND** 命令输出目录必须位于 accepted index directory 之外
- **AND** 如果 accepted index path 不存在，必须在执行前失败。

#### Scenario: 查询实验不得使用构建路径
- **WHEN** E2E benchmark 作为本 change 的一部分运行
- **THEN** 它不得在正式 query run 中构建新的 VecFetch 索引
- **AND** 任何 `index_source=rebuilt` 的 `results.json` 都必须被视为本次正式 Pareto 结果面的无效行，除非显式标记为本 change 之外的历史 probe。

### Requirement: 正式 VecFetch E2E 输出必须保留 Pareto 归因所需机制和资源字段
E2E benchmark 输出必须保留解释 recall-latency 点和支持后续资源分析所需的机制级字段。

#### Scenario: 输出 query latency 和 recall 字段
- **WHEN** 一个正式 Full VecFetch E2E 运行完成
- **THEN** 输出必须包含 `recall_at_1`、`recall_at_5`、`recall_at_10`、`recall_at_k`、`avg_query_time_ms`、`p50_query_time_ms`、`p95_query_time_ms` 和 `p99_query_time_ms`。

#### Scenario: 输出候选决策和读取字段
- **WHEN** 一个正式 Full VecFetch E2E 运行完成
- **THEN** 输出必须包含 SafeIn、SafeOut、Uncertain、Stage2 reclassification counts、vector-only read requests、all-read requests、payload read requests、remaining payload fetches，以及可用的 bytes 或 index footprint 字段。

#### Scenario: 输出 submit-path 字段
- **WHEN** 一个正式 Full VecFetch E2E 运行完成
- **THEN** 输出必须包含 `avg_probe_submit_ms`、`avg_probe_submit_vec_only_emit_ms`、fixed vector buffer hit/miss counts、`io_queue_depth` 和 `fixed_vec_buffer_count`。

#### Scenario: 输出 resident preload 字段
- **WHEN** 一个正式 Full VecFetch E2E 运行使用 resident full-preload 模式
- **THEN** 输出必须包含 preload time、preload bytes、resident cluster memory bytes，以及查询路径是否使用 resident clusters。

### Requirement: 正式 VecFetch E2E 控制项必须可从 config 输出重建
E2E benchmark 的配置输出必须让可复现实验所需的每个正式控制项都出现在 `config.json` 或 aggregate row 中。

#### Scenario: 记录 dynamic boundary 控制项
- **WHEN** 一个正式 Full VecFetch E2E 运行完成
- **THEN** 输出必须记录 Dynamic SafeOut 状态、Dynamic SafeIn mode、deferred frontier controls、SafeIn threshold bytes，以及所有 SafeIn/SafeOut epsilon override 字段。

#### Scenario: 记录 serving-mode 控制项
- **WHEN** 一个正式 Full VecFetch E2E 运行完成
- **THEN** 输出必须记录 `clu_read_mode`、resident-cluster usage、`prefetch_depth`、`io_queue_depth`、`fixed_vec_buffer_count`、`submission_mode` 和 `cluster_submit_reserve`。

#### Scenario: 记录索引和 payload 控制项
- **WHEN** 一个正式 Full VecFetch E2E 运行完成
- **THEN** 输出必须记录 resolved index directory、payload mode、payload source paths 或 source byte count、`nlist`、`nprobe`、`bits`、assignment mode、coarse builder、metric 和 rotation mode。
