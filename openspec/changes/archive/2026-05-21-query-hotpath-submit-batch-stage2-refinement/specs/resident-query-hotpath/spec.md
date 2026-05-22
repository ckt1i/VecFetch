## ADDED Requirements

### Requirement: Resident hot path SHALL support fixed-parameter three-layer submit control
在 `full_preload + use_resident_clusters=1` 的 resident hot path 中，轻量候选提交路径 MUST 支持固定参数版的三层 flush 机制，并将其视为 steady-state 的正式提交控制方式。该机制 MUST 在可复用 query wrapper / scratch 条件下工作，而不是通过重新构造通用提交流程来实现。

#### Scenario: Resident thin path uses three-layer submit control
- **WHEN** resident 查询路径在受控模式下运行
- **THEN** 它 MUST 使用 fixed-parameter `hard flush / soft flush / tail flush` 控制提交
- **AND** 该控制 MUST 运行在 resident thin path 内部

#### Scenario: Resident hot path can retain wrapper reuse under adaptive submit
- **WHEN** resident 查询路径切换到三层 flush 提交
- **THEN** query wrapper / scratch 的复用语义 MUST 保持有效
- **AND** 不得因为新的提交调度而退回到为通用路径准备的厚包装流程

### Requirement: Resident hot path SHALL support stop-sensitive flush for CRC early-stop
在 resident hot path 的 CRC early-stop 路径中，系统 MUST 支持 stop-sensitive flush 控制。该控制 MUST 在 cluster-end stop 判定前提供 `stop-safe flush`，并在决定提前停止后于退出 probe loop 前执行 `flush + drain`，同时保持 resident thin path 语义与 wrapper 复用语义不变。

#### Scenario: Resident early-stop path can flush before CRC check
- **WHEN** resident 查询路径在 CRC early-stop 模式下完成一个 cluster 且即将做 stop 判定
- **THEN** 它 MUST 允许执行 `stop-safe flush`
- **AND** 该动作 MUST 仍运行在 resident thin path 内部

#### Scenario: Resident early-stop path drains before break
- **WHEN** resident 查询路径在 CRC early-stop 模式下决定 break probe loop
- **THEN** 它 MUST 在 break 前执行 `flush + drain`
- **AND** 不得因为该动作而退回到非 resident 的通用查询包装流程

### Requirement: Resident hot path SHALL keep online-observation submit tuning reversible
resident hot path 若引入在线观测版 submit 调度，MUST 保持 query-local、轻量且可关闭。关闭在线观测调度后，系统 MUST 无缝回退到固定参数版三层 flush，而不改变 resident thin path 的其余语义。

#### Scenario: Online tuning is optional in resident mode
- **WHEN** resident 查询路径未启用在线观测调度
- **THEN** 系统 MUST 继续使用固定参数版三层 flush
- **AND** resident thin path 的候选有效性、dedup 和 rerank 语义 MUST 保持不变

#### Scenario: Online tuning does not widen resident path scope
- **WHEN** 在线观测调度被启用
- **THEN** 它 MUST 仅影响 resident 受控模式下的 submit 时机
- **AND** 不得把该机制强制外溢到非 resident 或非 thin-path 路径
