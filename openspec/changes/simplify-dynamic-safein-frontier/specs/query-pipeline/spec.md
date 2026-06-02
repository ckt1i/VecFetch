## ADDED Requirements

### Requirement: OverlapScheduler SHALL 只支持 static 和 frontier Dynamic SafeIn 模式
查询管线 SHALL 只通过两种 runtime 模式暴露 Dynamic SafeIn：`static`/`off` 用于 legacy global SafeIn threshold 行为，`frontier` 用于 query-adaptive SafeIn prefetch。系统 MUST NOT 将 `frontier_cap`、`frontier_delay`、`frontier_stable`、`frontier_scale` 或 `frontier_blend` 作为受支持 runtime 模式暴露。

#### Scenario: Static 模式保持 legacy SafeIn 行为
- **WHEN** 搜索以 `static` 或 `off` 关闭 Dynamic SafeIn
- **THEN** SafeIn classification 和 payload prefetch MUST 使用 index 中已有的 global SafeIn threshold
- **AND** 搜索 MUST 不依赖 Dynamic SafeIn frontier state

#### Scenario: 已删除动态模式不可用
- **WHEN** 代码或 benchmark 尝试选择 `frontier_cap`、`frontier_delay`、`frontier_stable`、`frontier_scale` 或 `frontier_blend`
- **THEN** 配置 MUST 被拒绝，或在编译期失败，而不是静默映射到其他模式

### Requirement: Frontier Dynamic SafeIn SHALL 使用 lower-bound frontier 阈值
在 `frontier` 模式下，query-adaptive SafeIn threshold SHALL 在 frontier ready 后等于当前第 k 个 lower-bound frontier `F_lower`。该阈值 MUST NOT 包含 lambda interpolation、scale multiplication 或 cap-to-static 行为。

#### Scenario: Frontier 阈值等于 lower frontier
- **WHEN** `frontier` 模式已经观察到足够候选并构建 top-k lower-bound frontier
- **THEN** SafeIn prefetch threshold MUST 为 `F_lower`
- **AND** 该阈值 MUST 不依赖 `F_upper - F_lower`

#### Scenario: Frontier 模式没有 scale 参数
- **WHEN** 配置 `frontier` 模式
- **THEN** runtime configuration MUST NOT 包含 Dynamic SafeIn scale、lambda 或 cap-to-static value

### Requirement: Frontier Dynamic SafeIn SHALL 支持 deferred candidate reclassification
查询管线 SHALL 为 frontier 模式保留 deferred SafeIn candidate buffering。frontier ready 前或配置的初始 defer window 内观察到的候选 MUST 被 buffer，并在之后基于 frontier threshold 重新分类，再决定提交 vector-only 或 all-read payload prefetch request。

#### Scenario: 初始 clusters 可以延迟 SafeIn 判断
- **WHEN** `frontier` 模式配置了正数 initial defer cluster count
- **THEN** 这些初始 clusters 中的 candidates MUST 被 buffer，而不是立即决定 vector-only 或 all-read

#### Scenario: Frontier ready 后 flush deferred candidates
- **WHEN** frontier threshold 变为 ready
- **THEN** buffered candidates MUST 基于 `F_lower` 重新分类
- **AND** 满足 SafeIn prefetch 条件的 candidates MUST 作为 all-read plans 提交
- **AND** 其余 candidates MUST 作为 vector-only plans 提交

#### Scenario: Final drain flushes deferred candidates
- **WHEN** query 结束时仍存在 deferred candidates
- **THEN** scheduler MUST flush 所有 deferred candidates
- **AND** 任何 candidate MUST NOT 因 dynamic frontier 一直未 ready 而被丢弃
