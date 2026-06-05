## Why

论文后续不再采用 probing 阶段的 CRC 校验进行 early-stop，继续保留这一路径会让查询逻辑、benchmark 口径和 SafeOut 行为解释变得混杂。当前 CRC early-stop 已经不是核心算法假设，应该从查询热路径和测试矩阵中移除，只保留不影响旧索引读取的兼容字段。

## What Changes

- 移除查询/probing 阶段的 CRC early-stop：在线搜索不再构造 `CrcStopper`，不再维护 CRC kth-distance heap，也不再调用 `ShouldStop()` 中断 probe。
- 移除 legacy 查询 early-stop：不再用 `collector.Full() && TopDistance() < conann.d_k()` 提前停止 probe，固定执行用户配置的 `nprobe`。
- 保留 dynamic SafeOut：SafeOut 继续使用独立的 query-time frontier，不依赖 `crc_params` 或 `--crc`。
- 停止在线 benchmark 消费 CRC calibration：`bench_vector_search` 和 `bench_e2e` 不再需要 `crc_scores.bin` 或 `crc_calibration_params.bin` 才能运行 SafeOut。
- 停止新建索引时默认生成 probing CRC 工件：builder 不再为查询 early-stop 生成 `crc_scores.bin`；旧索引中已有的 `crc_params` 字段和 `crc_scores.bin` 可被忽略。
- 清理 CRC early-stop 相关测试、benchmark、CMake target 和输出字段；如果下游脚本仍依赖字段名，字段可以短期保留为 deprecated/固定值，但不得驱动查询行为。
- 不移除 candidate-level SafeIn 校准逻辑；若后续论文需要完全去掉 “CRC” 命名，应另开变更重命名或替换该路径。

## Capabilities

### New Capabilities

- None.

### Modified Capabilities

- `crc-early-stop`: 废弃在线 CRC early-stop 能力，查询路径不得再根据 CRC 判定中断 probing。
- `early-stop`: 废弃查询阶段 early-stop 配置和 legacy `d_k` early-stop 行为。
- `crc-calibration`: CRC calibration 不再是在线查询或 benchmark 的必需输入，旧工件只作为兼容残留。
- `rabitq-crc-calibration`: RaBitQ CRC calibration 不再服务 probing early-stop，相关测试和 benchmark 路径需要删除或隔离。
- `dynamic-safeout`: 明确 SafeOut frontier 与 CRC 解耦，未提供 CRC 参数时仍必须正确执行 SafeOut。
- `benchmark-infra`: benchmark CLI、JSON 输出、脚本和测试不再要求或报告 probing CRC early-stop 作为正式功能。

## Impact

- 查询热路径：`SearchConfig`、`SearchStats`、`OverlapScheduler`、异步 cluster finalize/merge 逻辑。
- CRC 组件：`CrcStopper`、`CrcCalibrator` 的 probing early-stop 用途、`crc_scores.bin` 生成/读取路径。
- Benchmark：`bench_vector_search.cpp`、`bench_e2e.cpp`、CRC overlap/inline/recompute 工具、相关脚本和输出字段。
- 测试：CRC stopper、CRC calibrator、early-stop、OverlapScheduler 中依赖 CRC 的测试需要删除、改写或替换为 SafeOut 解耦测试。
- 存储兼容：不要求重建旧索引；旧 `segment.meta.crc_params` 字段和 `crc_scores.bin` 可继续存在但新查询路径忽略它们。
