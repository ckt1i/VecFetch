## REMOVED Requirements

### Requirement: CRC online early-stop decision
**Reason**: 论文和正式 benchmark 后续不再采用 probing 阶段 CRC 校验进行 early-stop；继续保留在线停止判定会混淆 fixed-`nprobe` 查询口径。

**Migration**: 查询路径 SHALL 固定执行配置的 `nprobe`。如果需要历史对比，应使用旧 commit 或 legacy diagnostic 工具，而不是正式查询 pipeline。

#### Scenario: CRC 参数不会触发 probe 中断
- **WHEN** 查询配置或旧索引中存在 CRC calibration 参数
- **THEN** 查询 pipeline MUST NOT 构造在线 `CrcStopper`
- **AND** 查询 pipeline MUST NOT 调用 CRC `ShouldStop()` 来提前结束 probing

#### Scenario: CRC estimate heap 不再参与查询热路径
- **WHEN** 查询执行 cluster probing 和候选扫描
- **THEN** 系统 MUST NOT 维护 CRC-specific kth estimate heap
- **AND** CRC estimate merge/buffer 统计 MUST NOT 作为正式查询统计输出

#### Scenario: 所有 probe 按固定 nprobe 执行
- **WHEN** 用户配置 `nprobe = N`
- **THEN** 查询 SHALL attempt probing the selected `N` clusters unless cluster selection itself yields fewer clusters
- **AND** probing MUST NOT be shortened by CRC confidence checks
