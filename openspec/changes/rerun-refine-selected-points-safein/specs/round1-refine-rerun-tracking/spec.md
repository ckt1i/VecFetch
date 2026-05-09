## ADDED Requirements

### Requirement: Round 1 跟踪必须产出固定文件
Round 1 补实验的跟踪必须产出稳定的 CSV 与汇总文件，避免与历史 thesis 主 sweep 混淆。

#### Scenario: 必需输出文件到位
- **WHEN** Round 1 重跑完成
- **THEN** 必须生成 `coco_main_alignment_round1.csv`
- **AND** 必须生成 `safein_repeat_coco_round1.csv`
- **AND** 必须生成 `safein_repeat_msmarco_round1.csv`
- **AND** 必须生成 `baseline_measurement_cleanup_round1.csv`
- **AND** 每个文件都与历史 thesis 主表 CSV 分离

#### Scenario: 必要决策汇总存在
- **WHEN** 关键补实验块全部完成或明确阻塞
- **THEN** 需生成 Round 1 决策汇总
- **AND** 汇总中写明 COCO matched-quality 选中点
- **AND** 汇总中写明 SafeIn 在论文中的表述方式
- **AND** 汇总中列明被阻塞或不支持的行

### Requirement: Round 1 跟踪保留可复现溯源
每条 Round 1 测量必须保留完整溯源，能支持重现与质量对齐复核。

#### Scenario: 公共字段记录
- **WHEN** 写入 Round 1 measurement 记录
- **THEN** 包含 dataset、system、variant、top-k、query 数、repeat id、phase、protocol
- **AND** 包含 canonical artifact 身份、coarse builder、`nlist`、`bits`、`nprobe`、GT 标识
- **AND** 包含 recall、avg latency、p50、p95、p99、QPS（有则记录）

#### Scenario: VecFetch 特有字段记录
- **WHEN** 写入 VecFetch 或 SafeIn 重复记录
- **THEN** 包含 `crc`、`early-stop`、SafeOut 数、Uncertain 数、SafeIn 数、reranked candidates、SafeIn prefetch count、final original-data fetch count、bytes read、submit calls 及 I/O wait/remaining-fetch 时间（有则记录）

#### Scenario: RaBitQ 特有字段记录
- **WHEN** 写入 `IVF+RaBitQ+FlatStor` 记录
- **THEN** 包含 `candidate_budget`
- **AND** 标明该行是原始 `candidate_budget=100` 对照，还是 Round 1 预算扫掠行
- **AND** 记录 canonical artifact 复用溯源，不仅仅是 index-cache 路径

### Requirement: Round 1 汇总采用重复分布统计
Round 1 汇总必须用重复分布作为主要证据，不将单次最佳作为核心结果。

#### Scenario: SafeIn 统计按重复分布输出
- **WHEN** 聚合 SafeIn 重复结果
- **THEN** 按 dataset、variant、top-k、`nprobe` 分组
- **AND** 对 avg latency 与 p99 latency 计算 mean/median/std/min/best
- **AND** 对 SafeIn prefetch、final original-data fetch、bytes read、reranked candidates 计算均值或中位数

#### Scenario: RaBitQ 清理行重复统计
- **WHEN** 聚合 RaBitQ 清理重复
- **THEN** 按 dataset、system、`nprobe`、`candidate_budget`、top-k 分组
- **AND** 对 recall 与延迟计算 mean/median/std/min/best
- **AND** 论文默认采用 median，除非决策汇总给出强制替代

### Requirement: COCO matched-quality 选点可追溯
COCO 选点输出必须显式说明选点规则和最终选定点。

#### Scenario: 几乎同 recall 点优先用于论文主表
- **WHEN** COCO 结果中存在 `R@10` 差异接近零的高召回对齐点
- **THEN** 论文主表优先采用该点
- **AND** 汇总必须记录双方 `nprobe`、`candidate_budget`、recall、avg、p95、p99 与加速比
- **AND** 中等召回下更高加速的窄带点可作为 Pareto 或补充分析，而不是替代主表

#### Scenario: 优先执行共同阈值选择
- **WHEN** COCO 选点结果可用
- **THEN** 共同阈值规则作为审计性 sanity check 执行
- **AND** 被选行保留 `nprobe`、`candidate_budget`、median recall、median latency 与重复统计

#### Scenario: 记录窄带回退
- **WHEN** 几乎同 recall 点不可用且共同阈值规则对所有系统都不可用
- **THEN** 改用窄带选择，要求 median `R@10` 差异 ≤ `0.010`
- **AND** 优先使用更严格的 `0.005`
- **AND** 汇总文件注明阈值规则未能满足的原因

### Requirement: SafeIn 论文表述受重复结果约束
SafeIn 结论必须由重复统计驱动，不得用单次现象写入正文。

#### Scenario: SafeIn 在方差内
- **WHEN** Full 与 SafeIn-off 中位 latency 差值小于重复方差
- **THEN** 论文表述为 SafeIn 在当前 top-10 条件下不显著
- **AND** SafeIn 不得作为摘要或结论中的主要收益来源

#### Scenario: SafeIn 受 I/O tail outlier 影响
- **WHEN** SafeIn 或 SafeIn-off 重复中出现显著 I/O tail outlier
- **THEN** 汇总必须保留异常留痕
- **AND** 正文采用保守表述，说明差异受尾延迟噪声影响
- **AND** 不得用该组结果支持 SafeIn 稳定增益

#### Scenario: SafeIn-off 稳定更快
- **WHEN** SafeIn-off 在 3 次重复中稳定快于 Full
- **THEN** 论文表述为 SafeIn 是可控预取路径，在低触发率下可能带来轻微额外开销
- **AND** 不将 SafeIn 作为当前主要来源

#### Scenario: Full 稳定更快
- **WHEN** Full 在 3 次重复中稳定快于 SafeIn-off
- **THEN** 论文可将 SafeIn 描述为次级增益
- **AND** 保持 SafeOut 与 Uncertain 为主机制
