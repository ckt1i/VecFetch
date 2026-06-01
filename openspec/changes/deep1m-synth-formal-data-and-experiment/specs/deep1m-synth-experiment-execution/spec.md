## ADDED Requirements

### Requirement: Deep1M_synth topk=10 experiment matrix

系统 SHALL 只在 `Deep1M_synth` validation gate 通过后，才执行 `topk=10` 主 sweep。

#### Scenario: Required topk=10 systems are scheduled

- **WHEN** 启动 `topk=10` 主 sweep 阶段
- **THEN** MUST 调度 VecFetch / BoundFetch-Guarded，nprobe 取值为 `16,32,64,128,256,512`
- **AND** MUST 调度 `IVF+RaBitQ+FlatStor`，使用同样的 nprobe 取值，且 `candidate_budget=100`
- **AND** MUST 调度 `IVF+PQ+FlatStor`，使用同样的 nprobe 取值，且 `candidate_budget=100`
- **AND** MUST 调度 `IVF+RaBitQ+Lance`，使用同样的 nprobe 取值，且 `candidate_budget=100`
- **AND** MUST 调度 `IVF+PQ+Lance`，使用同样的 nprobe 取值，且 `candidate_budget=100`

#### Scenario: Warmup precedes formal topk=10 measurement

- **WHEN** 执行一个正式的 `topk=10` measurement point
- **THEN** 在它之前 MUST 先完成一个等配置 warmup run
- **AND** 这个 warmup run MUST NOT 被计入论文统计
- **AND** measurement output MUST 记录 dataset、system、backend、nprobe、topk、candidate_budget、split id、canonical artifact path 和 protocol

### Requirement: Deep1M_synth baseline completeness

系统 SHALL 在 `Deep1M_synth` 结果被报告时，把四个 IVF baseline 组合视为必需项。

#### Scenario: Lance baseline is missing

- **WHEN** 需要输出 `Deep1M_synth` 结果，但 `IVF+RaBitQ+Lance` 或 `IVF+PQ+Lance` 缺失
- **THEN** 报告 MUST 将 `Deep1M_synth` 标记为 incomplete
- **AND** MUST NOT 把仅有 FlatStor 的结果包装成完整的 `Deep1M_synth` baseline family

#### Scenario: PQ recall is below common quality threshold

- **WHEN** PQ 无法达到共同 matched-quality 阈值
- **THEN** PQ FlatStor 与 PQ Lance 行 MUST 继续保留在输出表中，作为低质量 baseline 行
- **AND** matched-quality 决策 MAY 使用 VecFetch vs RaBitQ 的 narrow-band 选点规则

### Requirement: Deep1M_synth topk=10 matched-quality selection

系统 SHALL 根据完整 sweep 结果导出论文使用的 `Deep1M_synth topk=10` operating points。

#### Scenario: Common threshold succeeds

- **WHEN** VecFetch 与要求的 baseline family 都已经完成 `topk=10` sweep
- **AND** 至少 VecFetch 与 RaBitQ 能到达一个稳定的共同阈值，例如 `R@10 >= 0.95`
- **THEN** selector MUST 在该质量阈值下，为每个保留系统选择 median latency 最低的点
- **AND** 所采用的规则 MUST 被记录为 common-threshold rule

#### Scenario: Narrow-band fallback is needed

- **WHEN** common threshold 无法产生可信的 matched-quality 点
- **THEN** selector MUST 寻找 `|delta R@10| <= 0.005` 的 VecFetch / RaBitQ 配对点
- **AND** 如果没有更严格的配对点，它 MAY 放宽到 `|delta R@10| <= 0.010`
- **AND** 它 MUST 记录实际使用的 fallback rule

#### Scenario: Matched-quality selection fails

- **WHEN** common-threshold 与 narrow-band 两种选点方式都失败
- **THEN** 系统 MUST 输出 recall-latency curve summary，而不是宣称单点 speedup
- **AND** 它 MUST 标记是否需要补测 `96,160,192` 这类额外 nprobe 点

### Requirement: Deep1M_synth topk=20 supplement

系统 SHALL 只在 `topk=10` sweep 完成且 `recall@20` 验证通过后，才执行 `Deep1M_synth topk=20` 补充实验。

#### Scenario: Topk=20 supplement is scheduled

- **WHEN** 启动 `topk=20` supplement 阶段
- **THEN** MUST 在选中的低/中/高质量区间调度 VecFetch，初始点位为 `32,64,128` 或 `64,128,256`
- **AND** MUST 在相同质量区间调度四个 IVF baseline 组合
- **AND** IVF baseline 的 `candidate_budget` MUST 从 `150` 开始，且仅在 candidate recall 不足时 MAY 回退到 `200`

#### Scenario: Recall@20 support is unavailable

- **WHEN** `topk=20` 的 runner 或 summarizer 无法产出有效的 `recall@20`
- **THEN** `topk=20` 阶段 MUST 在写出论文用 CSV 之前停止
- **AND** MUST NOT 用 `recall@10` 去替代 `recall@20`

### Requirement: Deep1M_synth cleanup repeats

系统 SHALL 把最终可见的 `Deep1M_synth` 点位转换为稳定的 repeat measurement。

#### Scenario: Topk=10 final points are cleaned up

- **WHEN** 已经选出 `topk=10` 的最终点
- **THEN** 每个被选中的 VecFetch 和 baseline 点 MUST 执行 `1` 次 warmup 加 `3` 次 measurement repeat
- **AND** 如果 `Deep1M_synth` 要进入正文或 supplement，cleanup MUST 覆盖 VecFetch 和四个 IVF baseline 组合

#### Scenario: Topk=20 final points are cleaned up

- **WHEN** `topk=20` 结果被选中用于报告
- **THEN** 每个被选中的 `topk=20` 点 MUST 执行 `1` 次 warmup 加 `3` 次 measurement repeat
- **AND** cleanup MUST 保持与 `topk=10` 相同的 baseline family completeness 规则

### Requirement: Deep1M_synth result aggregation

系统 SHALL 生成适合 thesis 决策与论文写作使用的 `Deep1M_synth` 结果产物。

#### Scenario: Aggregation completes

- **WHEN** formal runs 与 cleanup repeats 全部完成
- **THEN** aggregation MUST 写出 `deep1m_synth_main_sweep_top10.csv`
- **AND** MUST 写出 `deep1m_synth_matched_quality_top10.csv`
- **AND** MUST 写出 `deep1m_synth_recall_latency_curve_top10.csv`
- **AND** 如果 `topk=20` 被执行，MUST 写出 `deep1m_synth_topk20_supplement.csv`
- **AND** MUST 写出 `deep1m_synth_cleanup_repeat3.csv`
- **AND** MUST 写出 `DEEP1M_SYNTH_DECISION_SUMMARY.md`

#### Scenario: Aggregation reports statistics

- **WHEN** 某个最终点存在 repeat measurements
- **THEN** aggregation MUST 计算 mean、median、standard deviation、min 和 best latency
- **AND** 论文面向的代表值 MUST 默认使用 median，除非 decision summary 中显式说明了其它稳定代表值的理由

### Requirement: Deep1M_synth failure handling

系统 SHALL 把 smoke、failed 和 formal outputs 保持隔离。

#### Scenario: A formal run fails

- **WHEN** 某个 `Deep1M_synth` formal run 失败
- **THEN** 失败记录 MUST 写明 dataset、system、backend、params 和 error
- **AND** failed outputs MUST NOT 被合并进论文面向的 CSV
- **AND** 如果该失败 run 属于 baseline completeness rule 所要求的必需项，则下一阶段 MUST 停止

#### Scenario: Smoke run succeeds

- **WHEN** 某个 smoke run 成功
- **THEN** 它的输出 MAY 在 validation report 中被引用
- **AND** 它 MUST NOT 被计为 formal measurement repeat
