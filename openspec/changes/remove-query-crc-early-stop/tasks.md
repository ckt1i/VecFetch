## 1. 查询配置与统计清理

- [x] 1.1 从 `SearchConfig` 中移除或废弃 `early_stop`、`crc_params`、`crc_no_break` 的正式查询语义。
- [x] 1.2 从 `SearchStats` 和 benchmark 聚合逻辑中移除 CRC early-stop 专用统计，或将短期兼容字段固定为 deprecated/no-op。
- [x] 1.3 确认 dynamic SafeOut 的配置项和统计字段仍保留，并且不依赖 CRC 配置。

## 2. OverlapScheduler 热路径移除

- [x] 2.1 删除 `OverlapScheduler` 中的 `CrcStopper`、CRC estimate heap、`use_crc_` 和 `AddCrcEstimate()` 相关状态。
- [x] 2.2 删除普通 probing 路径中的 CRC `ShouldStop()` 分支。
- [x] 2.3 删除 resident/full-preload probing 路径中的 CRC `ShouldStop()` 分支。
- [x] 2.4 删除 legacy `collector.Full() && TopDistance() < conann.d_k()` early-stop 分支，保证 fixed-`nprobe` 口径。
- [x] 2.5 将 candidate estimate buffering 条件从 CRC/SafeOut 共用改为仅由 dynamic SafeOut frontier 需要时启用。
- [x] 2.6 保留并验证 `AddSafeOutFrontierEstimate()`、`SafeOutFrontierUpper()` 和 SafeOut frontier 统计。

## 3. CRC 工件与索引兼容

- [x] 3.1 停止新建正式索引时为 probing CRC early-stop 设置 `crc_top_k`。
- [x] 3.2 停止 builder 为正式查询路径生成 `crc_scores.bin`，或将该生成路径移到 legacy/diagnostic 入口。
- [x] 3.3 保留 FlatBuffers `CrcParams` 字段解析兼容，不要求旧索引重建。
- [x] 3.4 验证没有 `crc_scores.bin` 的索引仍可打开并执行 vector search。

## 4. Benchmark 与脚本清理

- [x] 4.1 从 `bench_vector_search.cpp` 中移除 `--crc`、`--early-stop`、`--crc-*` 参数的正式行为和 Phase C.5 CRC calibration。
- [x] 4.2 从 `bench_e2e.cpp` 中移除 CRC sidecar/runtime calibration 读取和 early-stop 配置传递。
- [x] 4.3 清理 JSON/summary 输出中的 active CRC early-stop 字段，或将兼容字段固定为 deprecated/no-op。
- [x] 4.4 从默认 CMake/benchmark workflow 中移除 `recompute_crc_scores`、CRC overlap、CRC inline 等 probing CRC diagnostic target。
- [x] 4.5 更新 benchmark 脚本和文档，确保默认命令不再传入 `--crc` 或 `--early-stop`。

## 5. 测试调整

- [x] 5.1 删除或迁移只验证 `CrcStopper::ShouldStop()` 的单元测试。
- [x] 5.2 删除或迁移只验证 probing CRC calibration 的测试。
- [x] 5.3 更新 `early_stop_test`，不再期望 query early-stop 触发。
- [x] 5.4 保留或新增 OverlapScheduler 测试，验证无 CRC 参数时 dynamic SafeOut 仍可工作。
- [x] 5.5 确认 candidate-level SafeIn calibration 测试未被本变更误删。

## 6. 验证

- [x] 6.1 运行相关 C++ 单元测试，确认查询、SafeOut、builder 和 benchmark 编译通过。
- [x] 6.2 使用 COCO100k `index_fkmeans_2048_bits4_eps0.90` 在不输入 `--crc` 的情况下运行 `bench_vector_search`，确认 SafeOut 正常生效。
- [x] 6.3 对比删除前后的 `nprobe=64` 查询日志，确认 probing 不再出现 CRC/legacy early-stop，且 SafeIn/SafeOut/Uncertain 统计仍输出。
- [x] 6.4 记录验证命令、commit/构建目录和关键输出，作为后续论文实验口径说明。
