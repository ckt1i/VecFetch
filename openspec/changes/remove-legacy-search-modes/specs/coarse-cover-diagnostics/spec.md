## MODIFIED Requirements

### Requirement: assignment-mode 对照必须使用统一契约导出
系统 SHALL 不再要求在同一输出 schema 下支持 `single`、`redundant_top2_naive` 和 `redundant_top2_rair` 三种 assignment mode 的 pure coarse-cover 诊断。正式 coarse-cover 诊断 SHALL 以 single-assignment partition 为主线口径，并保留足够元数据确认该运行未使用 redundant 或 RAIR assignment。

#### Scenario: single assignment 诊断可复现
- **WHEN** 执行 pure coarse-cover 诊断运行
- **THEN** 输出 SHALL 明确记录该运行使用 single assignment
- **AND** 输出 SHALL NOT 要求同时存在 redundant top-2 或 RAIR 结果

#### Scenario: redundant 诊断维度被拒绝
- **WHEN** 诊断脚本或配置请求 `redundant_top2_naive`、`redundant_top2_rair` 或其他 overlap policy
- **THEN** 系统 SHALL 拒绝该诊断维度或标记为 legacy unsupported
- **AND** 它 SHALL NOT 将这些结果写入正式 coarse-cover 对照表
