# CRC Early Stop

## Description

CRC 在线早停判断：每 probe 一个聚类后，基于当前 top-k 状态和标定参数决定是否停止继续 probe。

## Requirements

### 输入

- `CalibrationResults`: 离线标定输出 (λ̂, k_reg, λ_reg, d_min, d_max)
- `probed_count`: 已 probe 的聚类数 (从 1 开始)
- `current_kth_dist`: 当前 top-k 堆的第 k 近距离 (堆未满时为 +∞)

### 输出

- `bool`: true = 应该停止, false = 继续 probe

### 判断逻辑

```
nonconf = clamp((current_kth_dist - d_min) / (d_max - d_min), 0, 1)
E = (1 - nonconf) + λ_reg · max(0, probed_count - k_reg)
reg_score = E / max_reg_val
return reg_score > λ̂
```

其中 `max_reg_val = 1 + λ_reg · max(0, nlist - k_reg)`，在构造时预计算。

### 边界情况

- **堆未满** (`current_kth_dist = +∞`): nonconf = 1.0, E = 0 + reg_term, reg_score 很小 → 不会触发早停。正确行为。
- **d_max == d_min**: 退化情况，所有 nonconf = 0 或 1。设 nonconf = 0（视为最优）。
- **probed_count == nlist**: 所有聚类已 probe，无需判断。

### 集成面

#### Benchmark (Phase 1)

自包含循环中直接调用 `CrcStopper::ShouldStop()`。

#### OverlapScheduler (Phase 2)

替换 `overlap_scheduler.cpp:258-265`:

```cpp
// 旧:
if (config_.early_stop &&
    ctx.collector().Full() &&
    ctx.collector().TopDistance() < index_.conann().d_k()) {
    ctx.stats().early_stopped = true;
    break;
}

// 新:
if (config_.early_stop &&
    crc_stopper_.ShouldStop(probed_count, ctx.collector().TopDistance())) {
    ctx.stats().early_stopped = true;
    ctx.stats().clusters_skipped = remaining;
    break;
}
```

`CrcStopper` 内部处理堆未满的情况（TopDistance = +∞ 时不会触发早停），因此不需要外部 `collector().Full()` 守卫。

### 约束

- ShouldStop 必须是 O(1) 操作（无堆分配、无循环）
- CalibrationResults 在查询前加载，查询过程中不可变
- 向量级三分类 (SafeIn/SafeOut/Uncertain) 在 ProbeCluster() 中保持不变，与 CRC 聚类级早停正交

## ADDED Requirements (from decouple-crc-safeout-frontier)

### Requirement: CRC early-stop SHALL be independent from dynamic SafeOut
CRC early-stop SHALL only control whether the probe loop may terminate early via `CrcStopper::ShouldStop()`. Dynamic SafeOut frontier maintenance and SafeOut candidate pruning SHALL NOT require CRC calibration parameters, `CrcStopper`, or probe-loop early stopping to be enabled.

#### Scenario: CRC disabled but dynamic SafeOut enabled
- **WHEN** query configuration disables CRC early-stop or provides no `crc_params`
- **AND** dynamic SafeOut is enabled
- **THEN** runtime MUST NOT call `CrcStopper::ShouldStop()`
- **AND** runtime MUST still maintain the dynamic SafeOut frontier state
- **AND** runtime MUST allow SafeOut pruning once the dynamic SafeOut frontier state is full

#### Scenario: early_stop disabled but dynamic SafeOut enabled
- **WHEN** query configuration sets `early_stop=false`
- **AND** dynamic SafeOut is enabled
- **THEN** runtime MUST NOT break out of the probe loop due to CRC
- **AND** runtime MUST still maintain and apply dynamic SafeOut frontier pruning

#### Scenario: CRC enabled but dynamic SafeOut disabled
- **WHEN** CRC early-stop is enabled
- **AND** dynamic SafeOut is explicitly disabled
- **THEN** runtime MUST maintain the CRC kth-score state required by `CrcStopper`
- **AND** runtime MUST NOT use that CRC state to prune candidates as SafeOut

#### Scenario: CRC score state remains calibration-compatible
- **WHEN** CRC early-stop is enabled
- **THEN** the `current_kth_dist` passed to `CrcStopper::ShouldStop()` MUST come from a state whose score semantics match the CRC calibration scores
- **AND** runtime MUST NOT replace the CRC kth score with `kth(d_hat + e)` unless CRC calibration is explicitly changed to the same score space
