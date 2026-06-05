## REMOVED Requirements

### Requirement: Legacy IVF query early-stop
**Reason**: fixed-`nprobe` 是后续论文和 benchmark 的统一口径；legacy `collector.Full() && TopDistance() < conann.d_k()` 会在没有 CRC 的情况下继续提前停止 probing，破坏口径一致性。

**Migration**: 删除 `SearchConfig::early_stop` 对正式查询路径的控制作用。查询完成后仍执行已有 final drain、payload fetch 和 result assembly。

#### Scenario: legacy d_k 不会提前停止 probing
- **WHEN** top-k heap 已满且当前 top distance 小于 legacy `conann.d_k()`
- **THEN** 查询 pipeline MUST continue probing until configured `nprobe` is exhausted
- **AND** 查询 pipeline MUST NOT set `early_stopped` because of legacy `d_k`

#### Scenario: 查询结果不再包含 early-stop 行为统计
- **WHEN** 查询完成
- **THEN** SearchStats MUST NOT expose active early-stop counters as formal result metrics
- **AND** any retained compatibility fields MUST remain fixed/deprecated and MUST NOT affect query behavior

#### Scenario: 关闭 early-stop 不再是 SafeOut 前提
- **WHEN** 用户未提供 early-stop 或 CRC 配置
- **THEN** dynamic SafeOut MUST still be able to run according to its own enable flag and frontier state
