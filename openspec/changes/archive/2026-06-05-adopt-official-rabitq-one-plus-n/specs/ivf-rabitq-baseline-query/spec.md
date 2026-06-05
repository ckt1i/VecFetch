## ADDED Requirements

### Requirement: IVF-RaBitQ query SHALL support official 1+n indexes
IVF-RaBitQ query path SHALL load and search official `1+n` indexes using official Stage1/Stage2 estimator semantics before exact raw-vector rerank.

#### Scenario: Official compressed search precedes exact rerank
- **WHEN** query CLI 打开 official `1+n` index
- **THEN** compressed candidate generation 必须使用 official `1+n` Stage1/Stage2 scoring
- **AND** final top-k 必须继续使用 raw vector plane 做 exact rerank

#### Scenario: Candidate budget applies after official compressed scoring
- **WHEN** compressed search 完成
- **THEN** candidate set 必须按 configured `candidate_budget` 截断
- **AND** rerank 必须只处理该 bounded candidate set

### Requirement: Query metadata SHALL expose official estimator mode
Query outputs SHALL expose effective RaBitQ format and bit semantics so downstream experiment scripts can compare official and legacy runs correctly.

#### Scenario: Official query output includes format metadata
- **WHEN** official `1+n` query run completes
- **THEN** output metadata 必须包含 `rabitq_estimator_mode=official_1_plus_n`
- **AND** 必须包含 `rabitq_total_bits`
- **AND** 必须包含 `rabitq_ex_bits`
- **AND** 必须包含 `.clu` storage version

#### Scenario: Legacy query output is not merged as official
- **WHEN** legacy signed-magnitude query run completes
- **THEN** output metadata 必须标记 legacy estimator mode
- **AND** downstream aggregation 必须能防止 legacy `bits=4` 被合并为 official `total_bits=4`

### Requirement: Query CLI SHALL allow official/legacy format control
Query CLI SHALL allow callers to request default auto-detection or explicit official/legacy format validation.

#### Scenario: Auto mode detects index format
- **WHEN** 用户未指定 format enforcement
- **THEN** query CLI 必须根据 index metadata 自动选择 official 或 legacy estimator
- **AND** 输出必须记录实际选择的 estimator mode

#### Scenario: Explicit official mode rejects legacy index
- **WHEN** 用户要求 official `1+n` mode
- **AND** 输入 index 是 legacy signed-magnitude format
- **THEN** query CLI 必须失败并报告格式不匹配

#### Scenario: Explicit legacy mode rejects official index
- **WHEN** 用户要求 legacy mode
- **AND** 输入 index 是 official `1+n` format
- **THEN** query CLI 必须失败并报告格式不匹配
