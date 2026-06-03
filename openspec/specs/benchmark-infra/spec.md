# Spec: Benchmark Infrastructure

## 目录结构

- Benchmark 源文件位于 `benchmarks/` 顶层目录
- Unit test 源文件保留在 `tests/` 目录，不受影响

## 编译隔离

- `VDB_BUILD_BENCHMARKS=ON` 编译所有 benchmark targets
- `VDB_BUILD_BENCHMARKS=OFF`（默认）不编译任何 benchmark
- `VDB_BUILD_TESTS` 和 `VDB_BUILD_BENCHMARKS` 互相独立，可任意组合
- 支持 `cmake --build build --target <bench_name>` 单独编译指定 benchmark

## benchmarks/CMakeLists.txt

- 受 `VDB_BUILD_BENCHMARKS` 控制
- 每个 benchmark 独立 `add_executable` + `target_link_libraries`
- 不注册 `add_test()`（benchmark 不参与 ctest）

## 根 CMakeLists.txt 改动

- `VDB_BUILD_TESTS` 块内不再包含 bench_* targets
- 在 tests 块之后添加 `add_subdirectory(benchmarks)`

## Additional Requirements (from coarse-builder-and-cover-diagnostics)

### Requirement: Benchmark 基础设施支持独立诊断输出
Benchmark 基础设施 SHALL 在 serving benchmark 之外支持独立的诊断运行，且诊断输出 SHALL 导出足够的结构化元数据，以支撑 builder 和 assignment-aware 的比较。

#### Scenario: 诊断 benchmark 输出包含 builder 元数据
- **WHEN** benchmark 以 diagnostic mode 运行
- **THEN** 输出 SHALL 包含 dataset、builder、assignment mode 以及 diagnostic metric family 等结构化元数据

#### Scenario: 诊断结果可以被聚合
- **WHEN** 生成了跨 builder 或 assignment mode 的多组诊断 benchmark 输出
- **THEN** 基础设施 SHALL 支持在无需手工重命名结果的情况下完成 summary 聚合

## Additional Requirements (from faiss-cpp-coarse-builder-integration)

### Requirement: Benchmark infrastructure records diagnostic metadata for coarse-builder runs
Benchmark 基础设施 SHALL 在 serving benchmark 之外支持独立的诊断运行，且诊断输出 SHALL 导出足够的结构化元数据，以支撑 builder 和阶段 gate-aware 的比较。

#### Scenario: 诊断 benchmark 输出包含 builder 与 gate 元数据
- **WHEN** 某次诊断运行完成并写出结果
- **THEN** 输出 SHALL 包含 dataset、builder identity、single/phase gate 状态、target metric family 以及 diagnostic run role 等结构化元数据

#### Scenario: Benchmark aggregation groups parity outputs by builder and gate
- **WHEN** 生成了跨 `hierarchical_superkmeans`、`superkmeans` 与 `faiss_kmeans` 的多组 parity 输出
- **THEN** 基础设施 SHALL 支持在无需手工重命名结果的情况下完成按 builder identity 与阶段 gate 的 summary 聚合

#### Scenario: Threshold summaries expose target probe levels
- **WHEN** 某个 builder parity 运行包含 target-threshold 聚合
- **THEN** summary 输出 SHALL 包含达到预设目标 `recall@10` 或 candidate-recall target 所需的最小 `nprobe`

#### Scenario: Faiss builder provenance is exported
- **WHEN** benchmark 或 build 输出涉及 `coarse_builder=faiss_kmeans`
- **THEN** 结构化元数据 SHALL 额外记录 clustering source、effective metric 和 Faiss training configuration
- **AND** 这些字段 SHALL 足以区分"C++ 进程内 Faiss 训练"与"导入预计算 Faiss artifacts"两种运行口径

## Additional Requirements (from single-coarse-builder-parity-optimization)

### Requirement: Benchmark 基础设施支持独立诊断输出
Benchmark 基础设施 SHALL 在 serving benchmark 之外支持独立的诊断运行，且诊断输出 SHALL 导出足够的结构化元数据，以支撑 builder 和阶段 gate-aware 的比较。

#### Scenario: 诊断 benchmark 输出包含 builder 与 gate 元数据
- **WHEN** benchmark 以 single coarse builder parity diagnostic mode 运行
- **THEN** 输出 SHALL 包含 dataset、builder identity、single/phase gate 状态、target metric family 以及 diagnostic run role 等结构化元数据

#### Scenario: 诊断结果可以按 builder 和阶段聚合
- **WHEN** 生成了跨 `hierarchical_superkmeans`、`superkmeans` 与 `faiss_kmeans` 的多组 parity 输出
- **THEN** 基础设施 SHALL 支持在无需手工重命名结果的情况下完成按 builder identity 与阶段 gate 的 summary 聚合

### Requirement: Benchmark summary SHALL report target-threshold operating points
Benchmark 基础设施 SHALL 支持为 single coarse builder parity 运行导出达到目标 `recall@10` 或等价 candidate-recall 指标所需的最小 `nprobe`。

#### Scenario: 输出达到目标 recall 所需最小 nprobe
- **WHEN** 一组 single coarse builder parity 运行覆盖了多个 `nprobe`
- **THEN** summary 输出 SHALL 包含达到预设目标 `recall@10` 或 candidate-recall target 所需的最小 `nprobe`

#### Scenario: 无法达到目标 threshold 时显式记录
- **WHEN** 所有已运行的 `nprobe` 都未达到预设 target threshold
- **THEN** summary 输出 SHALL 将该 builder / 阶段结果标记为 unreached，而不是静默省略

## ADDED Requirements (from probe-submit-fastpath-optimization)

### Requirement: Benchmark output SHALL expose probe-submit fast-path diagnostics
benchmark 输出 SHALL 提供 probe-submit fast path 的结构化诊断信息，以便将回归和收益归因到 request planning、slot allocation、SQE preparation、fixed-buffer availability 以及 rerank 向量存储上。

#### Scenario: Submit-path diagnostics are emitted
- **WHEN** 某次 query benchmark 完成，并且 submit-path 诊断可用或被启用
- **THEN** 输出 SHALL 包含 vector-only 请求数、all-read 请求数、payload 请求数、fixed-buffer 命中数、fixed-buffer 未命中数，以及 vector-only emit 计时
- **AND** 现有聚合字段如 `probe_submit_ms`、`probe_submit_prepare_vec_only_ms` 和 `uring_submit_ms` SHALL 保持可用

#### Scenario: Rerank vector storage diagnostics are emitted
- **WHEN** query benchmark 使用 buffered batch rerank
- **THEN** 输出 SHALL 包含 rerank 向量分配或 slab 计时、向量拷贝计时，以及 buffered candidate 数量
- **AND** 这些字段 SHALL 能够将分配开销与距离计算开销区分开来

### Requirement: Probe-submit optimization SHALL have benchmark acceptance coverage
实现 SHALL 包含可重复的 benchmark 覆盖，用于 MSMARCO `fht_kac_rotator` 的 query-only resident/full-preload 模式，并使用结构化指标对比优化前后的 submit 行为。

#### Scenario: MSMARCO resident query-only validation compares key metrics
- **WHEN** 优化后的路径与现有 `fht_kac_rotator` 索引进行对比
- **THEN** benchmark SHALL 报告 `avg_query_time_ms`、`probe_submit_ms`、`probe_submit_prepare_vec_only_ms`、`uring_submit_ms`、`io_wait_ms`、fixed-buffer 命中/未命中计数，以及 candidate 数量
- **AND** 如果有 ground truth 或确定性的 top-k baseline，可检查结果语义 MUST 与现有路径一致

## ADDED Requirements (from vec-only-buffer-lifecycle-fastpath)

### Requirement: CPU hot-path optimization benchmark SHALL report real recall
用于 CPU hot-path 优化结论的 benchmark SHALL 使用真实 ground truth，使输出中的 recall 字段可用。`query-only + skip-gt` 运行 MAY 用作热点定位或调试，但 MUST NOT 作为最终 recall 或正式性能结论的唯一依据。

#### Scenario: Optimization benchmark uses available ground truth
- **WHEN** 运行 MSMARCO `fht_kac_rotator` resident/full-preload 优化验证
- **THEN** benchmark SHALL 使用 external GT 或 computed GT
- **AND** 输出 SHALL 满足 `recall_available=true`
- **AND** 输出 SHALL 包含 `recall@1`、`recall@5` 和 `recall@10`

#### Scenario: Query-only profile is marked as diagnostic only
- **WHEN** benchmark 以 `query-only + skip-gt` 或等价方式运行
- **THEN** 该结果 SHALL 被视为 diagnostic-only
- **AND** 报告中 MUST NOT 将该结果的 zero recall 字段解释为算法 recall

### Requirement: Fixed vector buffer sweep SHALL expose lifecycle diagnostics
benchmark 基础设施 SHALL 支持对 fixed vector buffer count 进行 sweep，并导出足够的结构化指标来判断 vec-only buffer lifecycle fast path 的收益。

#### Scenario: Benchmark config records fixed vector buffer count
- **WHEN** benchmark 使用显式 fixed vector buffer count
- **THEN** 输出配置 SHALL 记录该 count
- **AND** 结果 SHALL 仍记录 `io_queue_depth`，以便区分 queue capacity 与 registered buffer capacity

#### Scenario: Lifecycle diagnostics are emitted for vec-only optimization
- **WHEN** resident vector-only buffer lifecycle fast path 被用于 query benchmark
- **THEN** 输出 SHALL 包含 fixed-buffer hit/miss、vector-only read request 数、payload/all read request 数、`probe_submit_ms` 和 `probe_submit_vec_only_emit_ms`
- **AND** 这些字段 SHALL 能够比较不同 fixed vector buffer count 下的 submit-path 行为

#### Scenario: Acceptance comparison keeps recall stable
- **WHEN** 比较优化前后或不同 fixed vector buffer count 的 benchmark 结果
- **THEN** recall 指标 SHALL 在同一 query 集和同一 ground truth 下比较
- **AND** 性能收益 MUST NOT 以 recall 回退为代价

## ADDED Requirements (from simplify-dynamic-safein-frontier)

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
- **WHEN** 文档或脚本描述已接受 Dynamic SafeIn 配置
- **THEN** 它们 MUST 使用 `--dynamic-safein frontier`
- **AND** MUST NOT 包含 lambda 或 scale argument

#### Scenario: Historical experiments are marked as historical
- **WHEN** 旧实验日志提到已删除 modes 或 `frontier_blend`
- **THEN** 它们 MUST 被更新为新的受支持命令，或被清晰标记为 historical experiment outputs
