## 1. 配置与状态解耦

- [x] 1.1 在 `SearchConfig` 中新增 dynamic SafeOut 独立开关，例如 `enable_dynamic_safeout`，并保留 `crc_params` 与 `early_stop` 的现有职责边界
- [x] 1.2 在 `OverlapScheduler` 中拆分状态命名：CRC early-stop state 只服务 `CrcStopper::ShouldStop()`，dynamic SafeOut state 只服务 `safeout_frontier_upper`
- [x] 1.3 确认 `early_stop=false` 时不调用 CRC break 逻辑，但不影响 dynamic SafeOut frontier 的维护
- [x] 1.4 确认 `crc_params == nullptr` 时 dynamic SafeOut 仍可启用，并且不需要构造 `CrcStopper`

## 2. SafeOut Upper-Bound Frontier

- [x] 2.1 实现 SafeOut frontier entry，记录 `d_hat`、`e`、`U=d_hat+e`
- [x] 2.2 实现按 `U` 排序的 size-`top_k` max-heap，保留当前 query 已见候选中 `U` 最小的 top-k
- [x] 2.3 当 SafeOut frontier heap 满时导出 `F=heap.top().U`，未满时导出 `+inf`
- [x] 2.4 在每个 query 开始时重置 SafeOut frontier state，避免跨 query 污染
- [x] 2.5 仅将通过当前 cluster 分类并进入 sink 的候选合并进 frontier，SafeOut 掉的候选不得进入 frontier
- [x] 2.6 Stage1 直接发出的候选使用 Stage1 `d_hat/e` 更新 frontier，Stage2 发出的候选使用 Stage2 `d_hat/e` 更新 frontier

## 3. CRC Early-Stop 独立路径

- [x] 3.1 保留 CRC early-stop 所需的 kth score state，并确保其 score 语义仍匹配 `crc_scores.bin` / `CrcCalibrator`
- [x] 3.2 当 CRC early-stop 与 dynamic SafeOut 同时启用时，同一批候选 estimate 分别更新 CRC state 和 SafeOut upper-bound state
- [x] 3.3 当 dynamic SafeOut 关闭但 CRC early-stop 开启时，CRC early-stop 仍能正常调用 `CrcStopper::ShouldStop()`
- [x] 3.4 当 CRC early-stop 关闭但 dynamic SafeOut 开启时，probe loop 必须完整走到 `nprobe`，但 SafeOut 剪枝仍生效

## 4. 分类路径接入

- [x] 4.1 在 `ProbeCluster` 入口处快照一次当前 `safeout_frontier_upper`，并在该 cluster 内复用同一个值
- [x] 4.2 确认 Stage1 FastScan SafeOut mask 使用 `d_hat > F + e_i`
- [x] 4.3 确认 Stage2 SIMD batch classifier 使用 `d_hat > F + e_i`
- [x] 4.4 确认 Stage2 scalar / split-margin fallback 使用 `d_hat > F + e_i`
- [x] 4.5 确认 SafeIn 判定继续使用 SafeIn 专用阈值，不使用 dynamic SafeOut frontier
- [x] 4.6 Dynamic SafeOut 显式关闭时，传入 `+inf` frontier 并保证 estimate-driven SafeOut 不触发

## 5. Benchmark 与输出

- [x] 5.1 在 `bench_vector_search.cpp` 增加 dynamic SafeOut 独立 CLI 参数，并在 JSON 中记录该开关
- [x] 5.2 在 `bench_e2e.cpp` 增加 dynamic SafeOut 独立 CLI 参数，并在 JSON/日志中记录该开关
- [x] 5.3 输出中区分 `crc_enabled`、`early_stop_enabled`、`dynamic_safeout_enabled`
- [x] 5.4 输出中保留 CRC early-stop 统计，同时新增或复用 SafeOut frontier 统计，避免把 SafeOut 剪枝误报为 CRC 行为
- [x] 5.5 检查 replay/diagnostic 工具中使用 SafeOut frontier 的公式说明，避免继续描述为 CRC-bound frontier

## 6. 测试

- [x] 6.1 增加 SafeOut frontier heap 单元测试，验证按 `U=d_hat+e` 选 top-k，而不是按 `d_hat` 选 top-k
- [x] 6.2 增加 heap 未满测试，验证 `F=+inf` 且 SafeOut 不触发
- [x] 6.3 增加 `crc_params=nullptr && enable_dynamic_safeout=true` 测试，验证 SafeOut frontier 能被维护
- [x] 6.4 增加 `early_stop=false && enable_dynamic_safeout=true` 测试，验证不 early-stop 但仍有 SafeOut 剪枝
- [x] 6.5 增加 `crc_params!=nullptr && enable_dynamic_safeout=false` 测试，验证 CRC early-stop 不依赖 SafeOut frontier
- [x] 6.6 增加 Stage2 SIMD 与 scalar SafeOut 判定等价性测试，覆盖 shared-margin 和 split-margin 路径

## 7. COCO100k 验证

- [x] 7.1 构建 `bench_vector_search`
- [x] 7.2 使用 COCO100k `nlist=2048,nprobe=64` 运行 dynamic SafeOut only 配置：CRC early-stop 关闭、dynamic SafeOut 开启
- [x] 7.3 运行 no-SafeOut 对照配置：CRC early-stop 关闭、dynamic SafeOut 关闭
- [x] 7.4 可选运行 CRC+dynamic SafeOut 配置，确认两者同时启用时统计字段不混淆
- [x] 7.5 记录 Stage1 SafeIn / SafeOut / Uncertain，以及 Stage2 SafeIn / SafeOut / Uncertain
- [x] 7.6 记录 recall、latency、false SafeOut、false SafeIn，并说明 `F=kth(d_hat+e)` 相比旧绑定路径的 SafeOut 数量变化
- [x] 7.7 将结果写入 `openspec/changes/decouple-crc-safeout-frontier/validation/`

## 8. 验收

- [x] 8.1 运行相关单元测试
- [x] 8.2 运行 `openspec validate decouple-crc-safeout-frontier --strict`
- [x] 8.3 确认 `--crc 0 --early-stop 0 --dynamic-safeout 1` 工作点能够产生 SafeOut
- [x] 8.4 确认 `--crc 1 --early-stop 0 --dynamic-safeout 1` 不触发 probe early-stop，但 SafeOut 仍生效
- [x] 8.5 确认 `--crc 1 --dynamic-safeout 0` 不产生 estimate-driven SafeOut，但 CRC early-stop 行为仍按 `CrcStopper` 决定
