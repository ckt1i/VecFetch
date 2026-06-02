## ADDED Requirements

### Requirement: Benchmarks SHALL 只暴露受支持的 Dynamic SafeIn CLI
配置 Dynamic SafeIn 的 benchmark tools SHALL 对 `--dynamic-safein` 只接受 `static`、`off` 和 `frontier`。它们 MUST 拒绝已删除的 mode names，并 MUST 删除用于配置已删除 scale、cap、gap 或 payload-only 行为的 CLI flags。

#### Scenario: 使用新名称选择 Frontier 模式
- **WHEN** 使用 `--dynamic-safein frontier` 调用 `bench_vector_search` 或 `bench_e2e`
- **THEN** benchmark MUST 配置 query pipeline 的 frontier Dynamic SafeIn 模式
- **AND** output configuration MUST 将 mode 记录为 `frontier`

#### Scenario: 旧 frontier_blend 命令被拒绝
- **WHEN** 使用 `--dynamic-safein frontier_blend` 调用 benchmark
- **THEN** benchmark MUST 以清晰的 invalid-argument error 拒绝该调用

#### Scenario: 已删除 Dynamic SafeIn knobs 不再被接受
- **WHEN** benchmark command 包含已删除的 Dynamic SafeIn flags，例如 scale、scale-cap-static、gap tolerance 或 payload-only
- **THEN** benchmark MUST NOT 将这些 flags 当作受支持配置处理

### Requirement: Benchmark outputs SHALL 只保留 frontier-relevant diagnostics
Benchmark logs、JSON 和 per-query CSV output SHALL 保留评估剩余 frontier 模式所需的 diagnostics，并 SHALL 删除仅描述已删除 modes 或 gates 的字段。

#### Scenario: Frontier diagnostics remain observable
- **WHEN** benchmark 以 `--dynamic-safein frontier` 完成
- **THEN** output MUST 包含 read counts、SafeIn payload-prefetch counts、remaining payload-fetch counts、deferred candidate counts、deferred flush counts、deferred SafeIn counts，以及 dynamic SafeIn threshold/frontier summaries

#### Scenario: 已删除 gate diagnostics 被移除
- **WHEN** 该 change 后写出 benchmark output
- **THEN** output MUST NOT 包含 Dynamic SafeIn gap-ready samples、gap averages、final gap、scale、scale-cap-static 或 payload-only fields

### Requirement: Experiment documentation SHALL 将已接受命令迁移到 frontier
仓库中的实验脚本和文档在描述已接受 Dynamic SafeIn 配置时 SHALL 使用 `--dynamic-safein frontier` 和固定 lower-frontier 语义。

#### Scenario: 文档记录 accepted default command
- **WHEN** 文档或脚本描述已接受的 Dynamic SafeIn 配置
- **THEN** 它们 MUST 使用 `--dynamic-safein frontier`
- **AND** MUST NOT 包含 lambda 或 scale argument

#### Scenario: Historical experiments are marked as historical
- **WHEN** 旧实验日志提到已删除 modes 或 `frontier_blend`
- **THEN** 它们 MUST 被更新为新的受支持命令，或被清晰标记为 historical experiment outputs
