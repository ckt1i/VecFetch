## Context

当前查询路径同时存在两类停止机制：一类是 legacy `d_k` early-stop，另一类是 CRC calibration 驱动的 probing early-stop。CRC 路径会在 benchmark 中读取 `crc_scores.bin` 或 legacy `crc_calibration_params.bin`，将 `CalibrationResults` 传入 `SearchConfig::crc_params`，再由 `OverlapScheduler` 维护 CRC estimate heap 并调用 `CrcStopper::ShouldStop()`。

SafeOut 已经有独立的 query-time frontier，可以在没有 `crc_params` 的情况下工作。后续论文不再使用 probing 阶段 CRC early-stop，因此继续保留 CRC 在线路径会增加 CPU 热路径复杂度，并混淆 fixed-`nprobe` 搜索与 early-stop 搜索的实验口径。

## Goals / Non-Goals

**Goals:**

- 在线查询固定执行配置的 `nprobe`，不再通过 CRC 或 legacy `d_k` 提前停止 probing。
- 删除或停用 CRC early-stop 在 `OverlapScheduler`、`SearchConfig`、`SearchStats`、benchmark CLI、JSON 输出、CMake target 和测试中的正式功能入口。
- 保留 dynamic SafeOut，并保证 SafeOut 不依赖 CRC 参数、CRC 文件或 `--crc` 开关。
- 保持旧索引可读：已有 `segment.meta.crc_params` 和 `crc_scores.bin` 不阻塞新 runtime 打开索引。
- 降低后续 FastScan/RaBitQ 单 bit 优化的耦合面，使优化只面对固定 probe 和 SafeIn/SafeOut/Uncertain 分类路径。

**Non-Goals:**

- 不删除 FlatBuffers schema 中已存在的 `CrcParams` 字段。
- 不要求重建已有 COCO100k/MSMARCO 索引。
- 不移除 candidate-level SafeIn 校准逻辑，也不在本变更中统一重命名所有 “CRC” 术语。
- 不改变 SafeIn 阈值校准、SafeOut frontier 判定公式或 RaBitQ 距离估计公式。

## Decisions

### Decision 1: 在线路径彻底移除 early-stop，而不是仅默认关闭

查询路径 SHALL 不再创建 `CrcStopper`，不再维护 CRC-specific estimate heap，也不再在 probe loop 内检查 `config.early_stop`。legacy `collector.TopDistance() < conann.d_k()` early-stop 也 SHALL 被移除，避免实验口径中仍存在隐式早停。

替代方案是保留代码并默认关闭，但这会继续保留 `SearchConfig::early_stop`、CRC stats 和 benchmark 参数，容易在后续测试中被误开，不利于论文口径收敛。

### Decision 2: SafeOut frontier 保留为独立机制

dynamic SafeOut SHALL 继续维护自己的 frontier，例如 top-k 估计上界 `kth(d_hat + e)` 或当前实现中的等价上界。用于 SafeOut 的 frontier 数据结构不得复用 CRC `crc_est_heap_`，也不得要求 `SearchConfig::crc_params` 非空。

这样可以保留 SafeOut 剪枝收益，同时删除 CRC early-stop 的 calibration 和停止决策。

### Decision 3: CRC 持久化采用兼容忽略策略

新 builder SHALL 不再为了 probing early-stop 生成 `crc_scores.bin`。旧索引若已有 `segment.meta.crc_params` 或 `crc_scores.bin`，新查询路径 SHALL 忽略它们。FlatBuffers schema 字段保留，避免破坏旧 metadata 的解析兼容性。

如果未来要彻底清理 schema，应另开存储格式迁移变更，并定义 metadata version、迁移工具和兼容测试。

### Decision 4: benchmark 输出从“功能字段”改为“无 CRC 口径”

正式 benchmark SHALL 不再接受 CRC early-stop 作为结果口径。`--crc`、`--early-stop`、`--crc-*` 等参数应删除或标记为 deprecated 且不影响查询行为。JSON 中与 CRC early-stop 直接相关的字段应删除；若下游脚本短期依赖字段，可保留固定值并标记 deprecated。

## Risks / Trade-offs

- [Risk] 删除 early-stop 后 query 时间可能上升，因为所有查询都会执行完整 `nprobe`。→ Mitigation: 验证时固定报告 `nprobe` 和 SafeOut/Uncertain 分布，确保性能变化来自口径变化而非 bug。
- [Risk] 下游脚本可能依赖 `crc_enabled`、`early_stop_rate` 等 JSON 字段。→ Mitigation: 第一轮可保留 deprecated 固定字段，后续再删除脚本依赖。
- [Risk] 误删 candidate-level SafeIn 校准路径。→ Mitigation: 本变更只删除 probing CRC early-stop；`candidate_safein_crc` 相关 artifact/CLI 需要明确排除。
- [Risk] 删除 `crc_calibrator` target 可能影响历史诊断工具。→ Mitigation: 优先从正式 benchmark 和测试矩阵移除；历史工具若仍需保留，应移动到 legacy/diagnostic 并不参与默认构建。

## Migration Plan

1. 查询路径先移除 `SearchConfig` 到 `OverlapScheduler` 的 CRC/early-stop wiring，并保留 SafeOut frontier 测试。
2. benchmark 停止读取 `crc_scores.bin` 和 `crc_calibration_params.bin`，默认命令行不再包含 `--crc` 或 `--early-stop`。
3. builder 默认不生成 `crc_scores.bin`，并移除 `crc_top_k` 在正式 build 配置中的设置。
4. 清理 CRC early-stop 单测、benchmark target 和脚本引用。
5. 在 COCO100k 上运行 fixed-`nprobe` vector search/e2e 验证，确认无 `--crc` 时 SafeOut 正常生效。

## Open Questions

- 是否在本轮直接删除 JSON 中所有 CRC 字段，还是先保留 deprecated 固定值以兼容现有分析脚本？
- `CrcCalibrator` 是否完全删除，还是暂时作为 legacy diagnostic target 保留但不参与默认构建？
