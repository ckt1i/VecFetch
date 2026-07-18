# E2E Benchmark Spec

## Overview

End-to-end benchmark infrastructure for vector search, supporting both sliding-window and full-preload cluster loading modes.
## Requirements
### Requirement: E2E benchmark output SHALL support `.clu` loading-mode comparison
The E2E benchmark workflow SHALL support direct before/after comparison between the existing sliding-window `.clu` loading path and the new full-preload `.clu` loading path under the same warm-serving query settings. 对 resident 主路径优化相关实验，benchmark 还 MUST 能在固定参数下输出同口径的 full E2E 结果与 query-only perf 结果，以支持主路径收益归因.

#### Scenario: Same operating point is run under both loading modes
- **WHEN** the benchmark evaluates the `.clu` preload optimization
- **THEN** it SHALL run the same dataset and search parameters under both `window` and `full_preload` modes
- **AND** the exported results SHALL make the loading mode explicit

#### Scenario: Query-speed comparison fields are exported
- **WHEN** a loading-mode comparison result is exported
- **THEN** the result SHALL include `recall@10`, `e2e_ms`, and `p99_ms`
- **AND** it SHALL include enough metadata to reconstruct the exact operating point being compared

#### Scenario: Preload cost is recorded alongside query results
- **WHEN** `full_preload` mode is benchmarked
- **THEN** the benchmark output SHALL record the preload time and preload-related resident memory or byte footprint
- **AND** these fields SHALL be reported alongside the query-speed result rather than omitted

### Requirement: Benchmark cleanup validation SHALL compare against pre-cleanup best results
After redundant feature removal, benchmark validation SHALL compare the cleaned implementation against the best pre-cleanup result available under the same dataset, index, and frozen query parameters. The validation MUST report recall, average latency or QPS, total probed count, and rerank or candidate count when available.

#### Scenario: Same-index warm validation is executed
- **WHEN** cleanup implementation is complete
- **THEN** validation MUST reuse existing indexes rather than rebuilding unless the index is unavailable or incompatible
- **AND** the benchmark record MUST include enough metadata to identify dataset, index path, active ex-bits, resident ex-bits, nprobe, topk, and two-level routing settings

#### Scenario: Cleanup result is compared with tolerance
- **WHEN** post-cleanup metrics are compared with pre-cleanup best metrics
- **THEN** `recall@10` absolute loss MUST NOT exceed `0.002`
- **AND** average latency MUST NOT regress by more than `3%` unless repeated runs show the difference is within noise
- **AND** QPS MUST NOT drop by more than `3%` unless repeated runs show the difference is within noise

### Requirement: Benchmark output SHALL remove abandoned feature fields
Benchmark output SHALL stop treating removed feature-specific fields as formal output schema. It MUST preserve official fields needed for frozen-path comparison and mechanism attribution.

#### Scenario: Removed feature fields are absent
- **WHEN** benchmark JSON or CSV output is generated after cleanup
- **THEN** fields dedicated only to Stage2 progressive pruning, Stage1 envelope skip, address sorting, and budgeted early submit MUST NOT be required
- **AND** parsers used by cleanup validation MUST not fail because those removed fields are absent

#### Scenario: Frozen-path fields are preserved
- **WHEN** benchmark output is generated after cleanup
- **THEN** it MUST still include recall, latency or QPS, preload memory when applicable, coarse select time, probe prepare time, Stage1 time, Stage2 time, submit time, total probed count, and rerank or candidate count when applicable

### Requirement: Online benchmark SHALL expose execution-mode selection
The online benchmark SHALL allow selecting the query execution mode from the command line. The default MUST remain the current overlap pipeline. A `serial-no-overlap` option MUST select the fully staged serial pipeline used for strong No Pipeline ablation.

#### Scenario: Default benchmark remains overlap mode
- **WHEN** the benchmark is run without an execution-mode option
- **THEN** it MUST use the existing overlap pipeline behavior
- **AND** existing scripts that do not request serial no-overlap MUST remain compatible

#### Scenario: Serial no-overlap mode is selectable
- **WHEN** the benchmark is run with `--execution-mode serial-no-overlap`
- **THEN** query execution MUST use the serial no-overlap query pipeline
- **AND** the output metadata MUST record `execution_mode=serial_no_overlap`

### Requirement: Benchmark output SHALL distinguish serial no-overlap from serial drain diagnostics
Benchmark output SHALL distinguish the strong Serial NoOverlap baseline from weaker diagnostics such as `serial_data_drains`. Results produced by `--serial-data-drain 1` alone MUST NOT be labeled as the strong No Pipeline baseline.

#### Scenario: Serial drain remains separately identified
- **WHEN** a run uses `--serial-data-drain 1` with default overlap execution mode
- **THEN** the output MUST record `serial_data_drains=true`
- **AND** the output MUST NOT record `execution_mode=serial_no_overlap`

#### Scenario: Strong No Pipeline is explicit
- **WHEN** a run is intended as the strong No Pipeline baseline
- **THEN** it MUST record `execution_mode=serial_no_overlap`
- **AND** it MUST record that async candidate-data overlap is disabled

### Requirement: Benchmark output SHALL report serial read and attribution fields
Benchmark output for serial no-overlap runs SHALL include fields that make the lack of overlap and the cost of serialized access visible. These fields MUST be sufficient to compare Full Pipeline and Serial NoOverlap under the same candidate and read counts.

#### Scenario: Serial timing fields are exported
- **WHEN** a serial no-overlap benchmark completes
- **THEN** the output MUST include average serial raw-vector read time
- **AND** it MUST include average serial full-record read time
- **AND** it MUST include average serial final payload read time
- **AND** it MUST include total query time and tail latency under the same schema as overlap runs

#### Scenario: Serial count fields are exported
- **WHEN** a serial no-overlap benchmark completes
- **THEN** the output MUST include raw-vector read request count and bytes
- **AND** it MUST include full-record read request count and bytes
- **AND** it MUST include final payload read request count and bytes
- **AND** it MUST include exact rerank candidate count

### Requirement: Pipeline ablation results SHALL be valid only under matched semantics
The pipeline ablation reporting workflow SHALL mark a Full Pipeline vs Serial NoOverlap pair as valid only when recall and mechanism counts match within the documented tolerance. Pairs with mismatched candidate generation or read semantics MUST be excluded from pipeline speedup claims.

#### Scenario: Matched pair is accepted
- **WHEN** Full Pipeline and Serial NoOverlap are run on the same dataset, index, queries, top-k, nprobe, active bits, resident bits, SafeOut epsilon, two-level routing and candidate budget
- **THEN** the summary workflow MAY report their latency/QPS delta as pipeline effect
- **AND** it MUST include recall, total probed, SafeOut/SafeIn/Uncertain, rerank count and read-count deltas

#### Scenario: Mismatched pair is rejected
- **WHEN** a Full Pipeline vs Serial NoOverlap pair has unexplained recall, candidate count, rerank count or read-count mismatch beyond tolerance
- **THEN** the summary workflow MUST mark the pair invalid for pipeline attribution
- **AND** it MUST not use that pair to support an independent pipeline contribution claim

### Requirement: E2E benchmark SHALL reject deleted budget and early-submit controls
Maintained benchmark entry points MUST NOT expose candidate-budget selection, budgeted speculative-vector prefetch, or SafeIn optional early-submit as supported controls. Commands that pass a deleted flag MUST fail clearly rather than silently running a different protocol.

#### Scenario: Candidate-budget flag is rejected
- **WHEN** a maintained benchmark command passes `--non-safeout-candidate-budget`
- **THEN** the benchmark MUST exit with a clear unsupported-option error
- **AND** it MUST NOT run a capped candidate-selection protocol

#### Scenario: Speculative prefetch flag is rejected
- **WHEN** a maintained benchmark command passes `--budgeted-prefetch-limit`
- **THEN** the benchmark MUST exit with a clear unsupported-option error
- **AND** it MUST NOT create speculative raw-vector requests

#### Scenario: SafeIn optional early-submit flag is rejected
- **WHEN** a maintained benchmark command passes `--safein-optional-io-early-submit-max-requests`
- **THEN** the benchmark MUST exit with a clear unsupported-option error
- **AND** it MUST not enable a mandatory-backlog bypass

### Requirement: E2E benchmark output SHALL omit deleted-path fields
New benchmark output MUST NOT emit configuration, counters, or derived metrics dedicated only to candidate budgets, speculative-vector prefetch, or SafeIn optional early-submit. It SHALL retain general correctness, I/O, span, payload, memory, and latency fields needed to evaluate the supported pipeline.

#### Scenario: New result schema contains no deleted-path metrics
- **WHEN** a benchmark writes configuration and result metadata after this change
- **THEN** the output MUST omit deleted budget, speculative-prefetch, and optional-early-submit fields
- **AND** it MUST retain recall, probed candidates, reranked candidates, supported read counters, and latency metrics

### Requirement: Cleanup SHALL be validated with a same-index no-cap regression
The benchmark workflow MUST compare a pre-cleanup run with all removed features disabled against a post-cleanup run using the same index, query/GT assets, search parameters, storage layout, cache protocol, warmup, and repeat ordering.

#### Scenario: No-cap cleanup preserves search semantics
- **WHEN** pre-cleanup and post-cleanup runs use the frozen no-cap operating point
- **THEN** recall, total probed candidates, candidates reranked, and final result semantics MUST be identical
- **AND** any difference in supported read counters or latency MUST be reported and explained before the change is accepted

## CLI Parameters

| 参数 | 类型 | 默认值 | 说明 |
|------|------|--------|------|
| `--dataset` | string | `/home/zcq/VDB/data/coco_1k` | 数据集路径 |
| `--queries` | int | 0 | query 数量限制 |
| `--output` | string | `/home/zcq/VDB/test/` | 输出基础目录 |
| `--nlist` | int | 32 | K-Means 聚类数 |
| `--nprobe` | int | 32 | 搜索探查聚类数 |
| `--topk` | int | 10 | Top-K |
| `--early-stop` | int | 1 | 启用 early stop (1=on, 0=off) |
| `--bits` | int | 1 | RaBitQ 量化位数 |
| `--block-size` | int | 64 | RaBitQ 块大小 |
| `--c-factor` | float | 5.75 | RaBitQ c 因子 |
| `--max-iter` | int | 20 | K-Means 最大迭代 |
| `--seed` | int | 42 | 随机种子 |
| `--page-size` | int | 4096 | 存储页大小 |
| `--p-for-dk` | int | 99 | d_k 校准百分位 |

## Behavior Constraints

- 所有参数的默认值必须与当前硬编码值一致，确保无参数调用时行为不变
- 新增 `GetFloatArg` 辅助函数支持 float 类型参数
- config.json 输出中包含所有参数值（含默认值）

## MODIFIED Requirements (from quantize-lut-fusion-simd-optimization)

### Requirement: E2E benchmark 必须报告 epsilon 验证元数据
warm-serving benchmark 的输出必须包含足够的元数据，用来区分构建阶段的 epsilon 输入和运行时加载到的 FastScan 边界。

#### Scenario: 输出中可见加载后的 epsilon
- **当** `bench_e2e` 在重建索引或预构建索引上运行时
- **则** 输出中必须包含加载后的运行时 `eps_ip`
- **并且** 输出中必须标明这次运行使用的是重建后的索引，还是通过 `--index-dir` 复用的索引

### Requirement: E2E benchmark MUST preserve prepare and Stage1 fine-grained observability for fused-kernel comparison
在 query 主路径继续推进 fused `quantize + lut_build` 和 Stage1 后续 SIMD 优化时，benchmark 输出 SHALL 保持 prepare 与 Stage1 的细粒度子项可见，以便同口径比较参考路径、保留两段版和融合版。

prepare 至少 MUST 包含：

- `probe_prepare_subtract_ms`
- `probe_prepare_normalize_ms`
- `probe_prepare_quantize_ms`
- `probe_prepare_lut_build_ms`

Stage1 至少 MUST 包含：

- `probe_stage1_estimate_ms`
- `probe_stage1_mask_ms`
- `probe_stage1_iterate_ms`
- `probe_stage1_classify_only_ms`
- `probe_submit_ms`

#### Scenario: Fused prepare is benchmarked under the same field schema
- **WHEN** 系统切换到 fused `quantize + lut_build` 路径
- **THEN** benchmark 输出 MUST 仍然保留 prepare 四段字段
- **AND** 这些字段 MUST 能与参考路径直接对比

#### Scenario: Stage1 follow-up SIMD work remains attributable
- **WHEN** survivor compaction、batch classify 或 submit-prep 被继续优化
- **THEN** benchmark 输出 MUST 仍然能区分 Stage1 子项和 submit CPU 成本
- **AND** 不得把 submit 开销重新混入 Stage1 内部字段

### Requirement: E2E benchmark 必须报告 Stage 1 和 Stage 2 的候选流统计
用于 epsilon 验证和 resident 主路径分析的 benchmark 输出 MUST 包含分析候选流与 query 主路径变化所需的统计，而不能只输出最终延迟。除现有 Stage 1 / Stage 2 候选计数外，resident 主路径运行还 MUST 导出 `coarse_select_ms`、`probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms`、`probe_classify_ms` 和 `probe_submit_ms`。

#### Scenario: 候选流统计被导出
- **当** 一次 epsilon 验证运行结束时
- **则** 输出中必须包含 `avg_total_probed`
- **并且** 输出中必须包含 Stage 1 `SafeOut`
- **并且** 输出中必须包含 Stage 2 `SafeOut`
- **并且** 输出中必须包含 Stage 2 `Uncertain`
- **并且** 输出中必须包含 `avg_probe_time_ms`

#### Scenario: Resident query-path breakdown is exported
- **WHEN** benchmark 在 resident 优化路径上运行
- **THEN** 输出中 MUST 包含 `avg_coarse_select_ms`
- **AND** MUST 包含 `avg_probe_prepare_ms`
- **AND** MUST 包含 `avg_probe_stage1_ms`
- **AND** MUST 包含 `avg_probe_stage2_ms`
- **AND** MUST 包含 `avg_probe_classify_ms`
- **AND** MUST 包含 `avg_probe_submit_ms`

## Additional Requirements (from warm-serving-experiment-plan)

### Requirement: E2E benchmark output SHALL support warm Pareto analysis
The E2E benchmark workflow SHALL support warm steady-state reporting as the primary analysis mode and SHALL produce output that is sufficient for recall-latency Pareto comparison and mechanism attribution.

#### Scenario: Warm operating-point sweep is reported
- **WHEN** the benchmark is run for the main post-baseline experiment phase
- **THEN** it SHALL report multiple BoundFetch operating points under the same warm protocol
- **AND** each point SHALL include `recall@10` and `e2e_ms`

#### Scenario: BoundFetch aggregate output captures multi-parameter sweep identity
- **WHEN** a BoundFetch operating point is added to the aggregate output
- **THEN** its parameter field SHALL be rich enough to distinguish `nprobe`, `crc-alpha`, `epsilon-percentile`, and `nlist`
- **AND** the encoding SHALL be stable enough to reconstruct the Pareto source of each point

#### Scenario: Baseline aggregate output captures baseline sweep identity
- **WHEN** a baseline operating point is added to the aggregate output
- **THEN** its parameter field SHALL preserve the baseline-side control parameter used for the sweep
- **AND** that field SHALL be sufficient to reconstruct one Pareto curve per baseline family

#### Scenario: Mechanism fields are preserved for attribution
- **WHEN** a BoundFetch warm result is exported
- **THEN** the benchmark output SHALL preserve mechanism-level fields needed for attribution
- **AND** those fields SHALL include the effective submission and probe breakdown needed to explain the result

#### Scenario: Output location is stable for plan execution
- **WHEN** the main warm benchmark workflow is run
- **THEN** its aggregate result SHALL be recorded in `baselines/results/e2e_comparison_warm.csv`
- **AND** the corresponding narrative interpretation SHALL be updated in `baselines/results/analysis.md`

#### Scenario: Appendix validation is separated from the main result
- **WHEN** queue-depth, submission mode, or SQPOLL checks are run
- **THEN** they SHALL be treated as supporting validation rather than the main benchmark objective
- **AND** the main result SHALL remain the warm recall-latency Pareto comparison

#### Scenario: Reporting fields are complete enough for decision gates
- **WHEN** a warm operating point is added to the aggregate output
- **THEN** it SHALL include the fields required to judge recall-latency tradeoff and mechanism cost
- **AND** those fields SHALL be sufficient to decide whether to continue or stop further optimization

## Additional Requirements (from rair-secondary-assignment-optimization)

### Requirement: E2E benchmark SHALL compare single, naive top-2, and RAIR top-2 under one protocol
The end-to-end benchmark workflow SHALL support direct comparison among `single`, `redundant_top2_naive`, and `redundant_top2_rair` under the same dataset, query count, `nlist`, `nprobe`, `epsilon`, and warm-serving mode.

#### Scenario: Three assignment modes are exported under the same protocol
- **WHEN** the benchmark evaluates the RAIR change
- **THEN** it SHALL be able to run and export results for `single`, `redundant_top2_naive`, and `redundant_top2_rair`
- **AND** the exported results SHALL make the assignment mode explicit

#### Scenario: RAIR parameters are exported with benchmark results
- **WHEN** a `redundant_top2_rair` operating point is exported
- **THEN** the exported record SHALL include the RAIR configuration needed to reproduce the run
- **AND** that configuration SHALL include at least `lambda`

### Requirement: Benchmark evaluation SHALL answer whether RAIR reduces high-recall probing demand
The benchmark plan SHALL not rely only on a fixed-`nprobe` latency comparison. It SHALL include a comparison that can determine whether RAIR reduces the probing demand required to reach the target high-recall region.

#### Scenario: High-recall comparison reports whether target recall is reached at lower probing demand
- **WHEN** `redundant_top2_rair` is compared against `redundant_top2_naive`
- **THEN** the benchmark results SHALL make it possible to determine whether the target recall region is reached with fewer probed clusters or a lower equivalent probing workload
- **AND** the conclusion SHALL be based on measured outputs rather than offline reasoning alone

#### Scenario: Fixed-nprobe comparison remains available
- **WHEN** the benchmark evaluates RAIR at a fixed `nprobe`
- **THEN** it SHALL still export recall, average latency, and tail latency for fair end-to-end comparison
- **AND** those results SHALL be attributable to the assignment mode used to build the index

### Requirement: Benchmark output SHALL expose the cost of RAIR-based redundant serving
The benchmark output SHALL include the fields needed to judge the trade-off of RAIR beyond recall alone, including probe expansion, duplicate-candidate behavior, preload cost, and build cost.

#### Scenario: Duplicate and deduplicated candidate cost is reported
- **WHEN** a redundant-assignment operating point is exported
- **THEN** the result SHALL include a measure before duplicate collapse and a measure after duplicate collapse
- **AND** those fields SHALL remain comparable between naive top-2 and RAIR top-2

#### Scenario: Build and preload overhead is reported
- **WHEN** a RAIR-built index is benchmarked
- **THEN** the exported result SHALL include build-time and preload-cost fields
- **AND** those fields SHALL be directly comparable with `single` and `redundant_top2_naive`

## ADDED Requirements (from resident-thinpath-batch-rerank-optimization)

### Requirement: 在线 E2E benchmark 不得执行 CRC calibration
`bench_e2e` 的在线 benchmark 运行 MUST 不将 CRC calibration 作为执行流程的一部分。在线 benchmark SHALL 直接加载预先生成的 CRC 参数或等价工件；若工件缺失，系统必须以明确错误或明确的非在线口径退出，而不是在同一轮在线 benchmark 中重新 calibration。

#### Scenario: CRC 工件存在时直接加载
- **WHEN** benchmark 运行且索引目录中存在 CRC 参数或等价工件
- **THEN** 系统必须直接加载这些工件并进入在线查询阶段

#### Scenario: CRC 工件缺失时不回退到在线 calibration
- **WHEN** benchmark 运行且缺少所需 CRC 工件
- **THEN** 系统不得在该次在线 benchmark 中执行 CRC calibration 作为隐式回退路径

### Requirement: 分阶段优化必须产出固定口径测速与 perf 结果
本次 resident thin path 与 batched rerank 优化实现 MUST 在第二阶段和第三阶段分别使用相同参数执行 benchmark 测速，并对各阶段结果执行 perf 采样，作为后续优化参考。

#### Scenario: 第二阶段输出测速与 perf
- **WHEN** 第二阶段 resident thin path 实现完成
- **THEN** 系统必须使用约定 benchmark 参数输出速度结果，并产出对应 perf 热点结果

#### Scenario: 第三阶段输出测速与 perf
- **WHEN** 第三阶段 batched rerank 实现完成
- **THEN** 系统必须使用与第二阶段一致的 benchmark 参数再次输出速度结果，并产出对应 perf 热点结果

### Requirement: E2E benchmark 必须输出新分段统计字段
在线 E2E benchmark MUST 输出至少以下字段：`probe_ms`、`prefetch_submit_ms`、`prefetch_wait_ms`、`safein_payload_prefetch_ms`、`candidate_collect_ms`、`pool_vector_read_ms`、`rerank_compute_ms`、`remaining_payload_fetch_ms`、`e2e_ms`、`num_candidates_buffered`、`num_candidates_reranked`、`num_safein_payload_prefetched`、`num_remaining_payload_fetches`。

#### Scenario: benchmark 输出新统计字段
- **WHEN** benchmark 完成一次查询轮次并写出结果
- **THEN** 结果中必须包含约定的新分段时间和候选计数字段

#### Scenario: benchmark 区分两类原始向量读取口径
- **WHEN** benchmark 输出 `prefetch_*` 与 `pool_vector_read_ms` 等字段
- **THEN** 结果必须能够区分 probe 阶段按 cluster 从磁盘预读原始向量到内存池的开销与 all-probe-done 后从内存池批量读取原始向量用于 rerank 的开销

## ADDED Requirements (from coarse-centroid-score-simd-optimization)

### Requirement: E2E benchmark SHALL preserve coarse score breakdown after SIMD coarse optimization
The benchmark output SHALL continue to expose a separate coarse breakdown after the centroid score kernel is optimized, so that coarse score gains can be isolated from query normalize and top-n selection.

#### Scenario: Coarse timing fields remain available after optimization
- **WHEN** `bench_e2e` runs on a build that includes the SIMD coarse centroid score kernel
- **THEN** the result output SHALL still include `avg_coarse_select_ms`
- **AND** it SHALL still include `avg_coarse_score_ms`
- **AND** it SHALL still include `avg_coarse_topn_ms`

#### Scenario: Benchmark can compare scalar and SIMD coarse score paths
- **WHEN** the coarse score implementation changes from scalar to SIMD
- **THEN** the benchmark output SHALL remain sufficient to compare optimization before and after under the same dataset and query parameters
- **AND** the comparison SHALL not require removing the existing coarse timing fields

## ADDED Requirements (from probe-submit-windowed-batch-prepread-optimization)

### Requirement: E2E benchmark SHALL expose windowed submit attribution
在线 E2E benchmark MUST 输出足以解释 windowed submit 收益的统计字段。结果中 MUST 至少能够反映 submit 调用次数、submit window 的 flush 次数以及每次 flush 的请求规模，从而区分"submit 次数下降"与"候选量变化"两类影响。

#### Scenario: Windowed submit fields are exported
- **WHEN** benchmark 在启用 windowed submit 的查询路径上运行
- **THEN** 输出中必须包含 submit 次数
- **AND** 必须包含 submit window 的 flush 次数
- **AND** 必须包含用于反映每次 flush 请求规模的统计字段

### Requirement: E2E benchmark SHALL expose batch prepare attribution
在线 E2E benchmark MUST 输出足以解释 batch `PrepRead` 收益的统计字段。结果中 MUST 至少能够区分 `vec_only` 批量 prepare、`all` 批量 prepare、批量 SQE 发射与 `io_uring_submit` 的成本边界。

#### Scenario: Batch prepare breakdown is exported
- **WHEN** benchmark 导出 `probe_submit_ms`
- **THEN** 输出中必须能够区分 `vec_only` 批量 prepare 成本
- **AND** 必须能够区分 `all` 批量 prepare 成本
- **AND** 必须能够区分批量 SQE 发射成本
- **AND** `uring_submit_ms` 仍然必须单独输出

## MODIFIED Requirements (from ip-exrabitq-two-stage-optimization)

### Requirement: Benchmark Must Support Clean Perf and Full E2E As Separate Truths
The benchmark system SHALL treat clean perf and full E2E as separate but equally required evaluation modes for query optimization work.

#### Scenario: Clean perf isolates query CPU hotspots
- **WHEN** benchmark runs in query-only mode with `fine_grained_timing=0`
- **THEN** it SHALL provide a hotspot profile that is not materially polluted by fine-grained timing instrumentation in prepare sub-stages

#### Scenario: Full E2E remains the end-to-end truth
- **WHEN** benchmark runs in the standard full E2E mode
- **THEN** it SHALL continue to report recall and end-to-end latency fields required to judge whether a kernel optimization improves real search latency

### Requirement: Benchmark Must Support Stage2 Optimization Decisions
The benchmark system SHALL provide enough information to decide whether `IPExRaBitQ` optimization should stop at Stage 1 or continue into storage-layout changes.

#### Scenario: Stage 1 decision uses clean perf
- **WHEN** Stage 1 `IPExRaBitQ` kernel work is completed
- **THEN** clean perf results SHALL be sufficient to determine whether `IPExRaBitQ` remains a significant hotspot

#### Scenario: Stage 2 decision also checks end-to-end latency
- **WHEN** clean perf shows kernel improvement
- **THEN** full E2E results SHALL also be used to determine whether further storage-layout work is still justified

## ADDED Requirements (from ipexrabitq-stage2-dualpath-optimization)

### Requirement: Benchmarking SHALL measure the packed-sign kernel route before batch boosting
The benchmark workflow SHALL evaluate the packed-sign dedicated Stage2 kernel route before Stage2 batch boosting is considered for implementation.

#### Scenario: Packed-sign kernel route gets a standalone benchmark pass
- **WHEN** this change is evaluated
- **THEN** the benchmark workflow SHALL include a standalone measurement pass for the packed-sign dedicated Stage2 kernel route before any batch boosting result is used for decision making

### Requirement: Benchmarking SHALL distinguish kernel gains from Stage2 structure gains
The benchmark workflow SHALL preserve enough output and profiling context to distinguish gains from the packed-sign kernel route and gains from the Stage2 batch boosting route.

#### Scenario: Perf result can be attributed to one route
- **WHEN** clean perf is collected for this change
- **THEN** the result SHALL remain sufficient to attribute whether the observed improvement came primarily from the Stage2 kernel path or from the Stage2 batch boosting structure

### Requirement: This change SHALL continue to use clean perf and full E2E together
The benchmark workflow for this change SHALL continue to use both clean perf and full E2E, so that Stage2 compute gains and end-to-end serving gains can both be judged.

#### Scenario: Both benchmark modes are produced
- **WHEN** a route in this change is evaluated
- **THEN** the evaluation SHALL include query-only clean perf
- **AND** it SHALL include the corresponding full E2E run under the formal serving parameters

## ADDED Requirements (from crc-discrete-threshold-solver)

### Requirement: E2E benchmark SHALL expose runtime CRC solver selection
The runtime CRC preparation path in `bench_e2e` MUST allow explicit selection of the CRC calibration solver so that `brent` and `discrete_threshold` can be compared under the same serving operating point. The benchmark default SHALL remain `brent`.

#### Scenario: Benchmark runs without a solver override
- **WHEN** `bench_e2e` runs with CRC enabled and no solver override is provided
- **THEN** it MUST use `brent` as the runtime calibration solver

#### Scenario: Benchmark runs with a discrete solver override
- **WHEN** `bench_e2e` runs with CRC enabled and `discrete_threshold` is selected
- **THEN** it MUST run runtime CRC calibration with the discrete solver
- **AND** it MUST report that solver choice in the benchmark output

### Requirement: E2E benchmark SHALL export CRC solver provenance fields
When runtime CRC calibration is performed, the benchmark output MUST include the solver provenance and internal cost fields needed to compare Brent and discrete threshold solving under the same index and query settings.

#### Scenario: Runtime CRC provenance is written to results
- **WHEN** `bench_e2e` completes a run that performs runtime CRC calibration
- **THEN** the exported result MUST include `crc_solver`
- **AND** it MUST include `crc_candidate_count`
- **AND** it MUST include `crc_solver_ms`
- **AND** it MUST include `crc_profile_build_ms`
- **AND** it MUST include `crc_objective_evals`

## ADDED Requirements (from exrabitq-storage-format-upgrade)

### Requirement: E2E benchmark SHALL expose ExRaBitQ storage version metadata
当 benchmark 在 `bits > 1` 的 ExRaBitQ 索引上运行时，输出 MUST 显式记录当前索引使用的 ExRaBitQ storage 版本或等价格式标识，以便区分旧的 byte-sign 布局和新的 packed-sign 布局。

#### Scenario: Benchmark output records storage version
- **WHEN** `bench_e2e` 在 ExRaBitQ 索引上完成一次运行并导出结果
- **THEN** 结果中必须包含 ExRaBitQ storage version 或等价格式标识
- **AND** 该字段必须足以区分 byte-sign 与 packed-sign 两种布局

### Requirement: Benchmark SHALL validate packed-sign format under both clean perf and full E2E
针对 ExRaBitQ storage 升级，benchmark MUST 同时提供 query-only clean perf 与 full E2E 两个口径的验收结果，以验证 packed-sign 是否真实降低了 Stage2 成本，并确认结果语义未退化。

#### Scenario: Query-only benchmark can attribute Stage2 benefit
- **WHEN** packed-sign 格式与旧格式进行 query-only perf 对比
- **THEN** 输出必须能够用于判断 Stage2 热点占比是否迁移
- **AND** 不得把该对比建立在被细粒度打点污染的口径上

#### Scenario: Full E2E benchmark validates semantic compatibility
- **WHEN** packed-sign 格式与旧格式进行 full E2E 对比
- **THEN** 输出必须包含 recall 和端到端时延字段
- **AND** 必须能够确认 packed-sign 未改变结果语义

### Requirement: Benchmark SHALL make rebuild / format provenance explicit
当 benchmark 运行在升级后的 ExRaBitQ storage 格式上时，输出 MUST 能区分该结果来自旧格式索引还是新格式重建索引，并为后续 perf / 实验解释提供格式来源信息。

#### Scenario: Output distinguishes rebuilt packed-sign index from legacy index
- **WHEN** benchmark 导出一条 ExRaBitQ 结果记录
- **THEN** 结果中必须能区分该运行使用的是 legacy byte-sign 索引还是 rebuilt packed-sign 索引
- **AND** 该区分必须可用于后续分析和对照

## ADDED Requirements (from ipexrabitq-compact-layout-rebuild)

### Requirement: E2E benchmark SHALL report compact-layout rebuild identity
The E2E benchmark output SHALL make it explicit whether a run used the legacy `v10 packed_sign` Stage2 layout or the rebuild-required `v11` compact layout.

#### Scenario: Benchmark output distinguishes `v10` and `v11`
- **WHEN** benchmark results are exported for Stage2 layout comparison
- **THEN** the output SHALL identify whether the index uses `v10 packed_sign` or `v11 compact layout`
- **AND** that identity SHALL be stable enough for before/after comparison

### Requirement: E2E benchmark SHALL compare compact-layout serving against the current low-overhead baseline
The benchmark workflow SHALL support direct comparison between the current low-overhead serving baseline and the rebuilt compact-layout serving path under the same query parameters.

#### Scenario: Same operating point is run before and after compact rebuild
- **WHEN** the compact layout change is evaluated
- **THEN** benchmark runs SHALL use the same dataset, `nlist`, `nprobe`, `topk`, query count, preload mode, resident mode, and CRC settings before and after rebuild
- **AND** the exported results SHALL remain directly comparable

### Requirement: E2E benchmark SHALL expose Stage2 benefit after compact rebuild
The benchmark output SHALL continue to expose Stage2 and end-to-end breakdown fields after compact rebuild, so that the effect of the new layout can be attributed.

#### Scenario: Stage2 and E2E fields remain available after compact rebuild
- **WHEN** benchmark runs on a `v11` compact index
- **THEN** the output SHALL still include `avg_query_ms`
- **AND** it SHALL still include `avg_probe_stage2_ms`
- **AND** it SHALL still include the existing low-overhead query-path timing fields needed to explain the result

## ADDED Requirements (from query-hotpath-submit-batch-stage2-refinement)

### Requirement: E2E benchmark SHALL formalize the hot-path follow-up experiment protocol
在线 E2E benchmark MUST 为本轮 `submit-batch` 与 Stage2 follow-up 提供稳定实验协议。该协议 MUST 支持在固定 operating point 下执行多次重复实验，并允许在同一批参数上导出 full E2E 与 query-only perf 结果，以区分真实收益与噪声。

#### Scenario: Follow-up experiment repeats are comparable
- **WHEN** 某个 `submit-batch` 或 Stage2 follow-up 变体被评估
- **THEN** benchmark 协议 MUST 允许在固定 operating point 下重复执行多次
- **AND** 输出 MUST 足以比较均值与离散度，而不是只保留单次结果

#### Scenario: Query-only and E2E results are both available
- **WHEN** follow-up 优化项进入正式评估
- **THEN** 同一 operating point MUST 同时可获得 query-only perf 结果与 full E2E 结果
- **AND** 两者 MUST 使用相同的查询参数和结果口径

### Requirement: E2E benchmark SHALL expose adaptive submit attribution
针对三层 flush `submit-batch`，benchmark 输出 MUST 足以解释 flush 机制本身是否带来收益，而不仅是最终总时间变化。输出 MUST 至少能够区分 flush 次数、flush 规模和 `probe_submit` 相关 CPU 成本。

#### Scenario: Adaptive submit fields are exported
- **WHEN** benchmark 在三层 flush 的 resident 路径上运行
- **THEN** 结果 MUST 包含 flush 次数或等价统计
- **AND** MUST 包含每次 flush 请求规模的聚合信息
- **AND** `probe_submit_ms` 仍然 MUST 作为总提交 CPU 成本单独可见

### Requirement: E2E benchmark SHALL preserve Stage2 follow-up attribution boundaries
针对 Stage2 `collect/scatter` follow-up，benchmark 输出 MUST 继续支持区分 Stage2 内部成本边界。若某轮运行开启细粒度归因，结果 MUST 足以区分 collect、kernel 与 scatter 的变化；若运行于低开销口径，结果 MUST 至少保留总 Stage2 字段并显式说明口径限制。

#### Scenario: Fine-grained Stage2 fields remain comparable when enabled
- **WHEN** benchmark 以细粒度 Stage2 归因模式运行
- **THEN** 输出 MUST 能够区分 Stage2 collect、kernel 与 scatter 的时间字段
- **AND** 这些字段 MUST 可用于比较 follow-up 优化前后的变化

#### Scenario: Low-overhead mode still declares attribution limits
- **WHEN** benchmark 运行于低开销统计模式
- **THEN** 输出 MUST 至少保留总 Stage2 时间字段
- **AND** MUST 让分析结果能够区分"字段为零"是因为未计量还是因为成本确实消失

## ADDED Requirements (from query-simd-consolidation-and-expansion)

### Requirement: E2E benchmark SHALL expose query SIMD capability toggles in exported results
The benchmark workflow SHALL make query-path SIMD capability toggles explicit in exported results so that SIMD-on, fallback, and mixed configurations can be compared under the same operating point. At minimum, exported metadata MUST be able to distinguish address-decode SIMD, rerank batched-distance SIMD, coarse-select SIMD, Stage2 block-first, and Stage2 batch-classify states when those capabilities are present.

When a newly introduced SIMD runtime path is split into Phase 1 and Phase 2, exported metadata MUST be able to distinguish whether the run used:

- fallback/reference
- Phase 1 conservative implementation
- Phase 2 stronger specialization

#### Scenario: SIMD toggle state is exported
- **WHEN** a benchmark run enables or disables a query-path SIMD capability
- **THEN** the exported result MUST include the corresponding capability state
- **AND** the result MUST remain identifiable as SIMD-on or fallback for later comparison

#### Scenario: Phase 1 and Phase 2 runtime paths are benchmarked
- **WHEN** a benchmark run compares a newly introduced SIMD runtime path across rollout phases
- **THEN** the exported result MUST indicate whether the run used fallback, Phase 1, or Phase 2
- **AND** that distinction MUST be sufficient to compare correctness and performance across phases

### Requirement: Benchmark validation SHALL support both query-only and full E2E SIMD comparison
The benchmark workflow SHALL support validating query-path SIMD capabilities under both `query_only` and `full_e2e` modes. Query-only runs MUST preserve stage breakdown observability, and full E2E runs MUST preserve recall comparison.

#### Scenario: Query-only SIMD validation is run
- **WHEN** a query-path SIMD capability is validated under `query_only`
- **THEN** the result MUST export the relevant stage breakdown fields needed to attribute the SIMD change
- **AND** those fields MUST remain comparable with the fallback path

#### Scenario: Full E2E SIMD validation is run
- **WHEN** a query-path SIMD capability is validated under `full_e2e`
- **THEN** the result MUST report recall and latency under the same operating point as the fallback path
- **AND** the result MUST be sufficient to determine whether semantics changed

#### Scenario: Phase 2 SIMD validation is run
- **WHEN** a newly introduced SIMD runtime path is validated in its Phase 2 form
- **THEN** the benchmark MUST still be able to compare it directly against both fallback and Phase 1 under the same operating point
- **AND** the result MUST be sufficient to determine whether Phase 2 provided benefit beyond Phase 1 without changing semantics

## MODIFIED Requirements (from query-simd-consolidation-and-expansion)

### Requirement: E2E benchmark 必须报告 Stage 1 和 Stage 2 的候选流统计
用于 epsilon 验证和 resident 主路径分析的 benchmark 输出 MUST 包含分析候选流与 query 主路径变化所需的统计，而不能只输出最终延迟。除现有 Stage 1 / Stage 2 候选计数外，resident 主路径运行还 MUST 导出 `coarse_select_ms`、`probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms`、`probe_classify_ms` 和 `probe_submit_ms`。

当 query-path SIMD 能力在细粒度诊断模式下被验证时，benchmark 输出还 MUST 保持 `probe_stage2_collect_ms`、`probe_stage2_kernel_ms`、`probe_stage2_scatter_ms` 等子项可见，并确保 current path 与 fallback path 在 `query_only` 与 `full_e2e` 两种模式下都能同口径比较。

#### Scenario: 候选流统计被导出
- **当** 一次 epsilon 验证运行结束时
- **则** 输出中必须包含 `avg_total_probed`
- **并且** 输出中必须包含 Stage 1 `SafeOut`
- **并且** 输出中必须包含 Stage 2 `SafeOut`
- **并且** 输出中必须包含 Stage 2 `Uncertain`
- **并且** 输出中必须包含 `avg_probe_time_ms`

#### Scenario: Resident query-path breakdown is exported
- **WHEN** benchmark 在 resident 优化路径上运行
- **THEN** 输出中 MUST 包含 `avg_coarse_select_ms`
- **AND** MUST 包含 `avg_probe_prepare_ms`
- **AND** MUST 包含 `avg_probe_stage1_ms`
- **AND** MUST 包含 `avg_probe_stage2_ms`
- **AND** MUST 包含 `avg_probe_classify_ms`
- **AND** MUST 包含 `avg_probe_submit_ms`

#### Scenario: Fine-grained Stage2 comparison is preserved
- **WHEN** benchmark 以细粒度诊断模式验证 query-path SIMD
- **THEN** 输出中 MUST 包含 `avg_probe_stage2_collect_ms`
- **AND** MUST 包含 `avg_probe_stage2_kernel_ms`
- **AND** MUST 包含 `avg_probe_stage2_scatter_ms`
- **AND** 这些字段 MUST 能在 current path 与 fallback path 之间直接比较

## ADDED Requirements (from nonpow2-padded-hadamard-rotation)

### Requirement: E2E benchmark MUST preserve prepare and Stage1 fine-grained observability for fused-kernel comparison
在 query 主路径继续推进 fused `quantize + lut_build` 和 Stage1 后续 SIMD 优化时，benchmark 输出 SHALL 保持 prepare 与 Stage1 的细粒度子项可见，以便同口径比较参考路径、保留两段版和融合版。对于 padded-Hadamard 评估，benchmark 输出还 MUST 显式报告 `logical_dim`、`effective_dim`、`padding_mode`、`rotation_mode`，并能区分 prepare 中的 rotation 成本与 quantize/LUT 成本。

prepare 至少 MUST 包含：

- `probe_prepare_subtract_ms`
- `probe_prepare_normalize_ms`
- `probe_prepare_quantize_ms`
- `probe_prepare_lut_build_ms`

Stage1 至少 MUST 包含：

- `probe_stage1_estimate_ms`
- `probe_stage1_mask_ms`
- `probe_stage1_iterate_ms`
- `probe_stage1_classify_only_ms`
- `probe_submit_ms`

#### Scenario: Fused prepare is benchmarked under the same field schema
- **WHEN** 系统切换到 fused `quantize + lut_build` 路径
- **THEN** benchmark 输出 MUST 仍然保留 prepare 四段字段
- **AND** 这些字段 MUST 能与参考路径直接对比

#### Scenario: Padded-Hadamard metadata is exported with query results
- **WHEN** benchmark 在 padded-Hadamard 候选路径上运行
- **THEN** 输出 MUST 包含 `logical_dim`、`effective_dim`、`padding_mode` 和 `rotation_mode`
- **AND** 输出 MUST 足以区分 baseline random rotation 与 padded Hadamard

### Requirement: E2E benchmark 必须输出新分段统计字段
在线 E2E benchmark MUST 输出至少以下字段：`probe_ms`、`prefetch_submit_ms`、`prefetch_wait_ms`、`safein_payload_prefetch_ms`、`candidate_collect_ms`、`pool_vector_read_ms`、`rerank_compute_ms`、`remaining_payload_fetch_ms`、`e2e_ms`、`num_candidates_buffered`、`num_candidates_reranked`、`num_safein_payload_prefetched`、`num_remaining_payload_fetches`。对于 padded-Hadamard 比较，输出还 MUST 支持记录 prepare-rotation 与 prepare-quant-lut 两个总览字段。

#### Scenario: benchmark 输出新统计字段
- **WHEN** benchmark 完成一次查询轮次并写出结果
- **THEN** 结果中必须包含约定的新分段时间和候选计数字段

#### Scenario: Padded-Hadamard comparison exposes rotation tradeoff
- **WHEN** benchmark 导出一条 padded-Hadamard 比较结果
- **THEN** 结果 MUST 至少包含 `prepare_rotation_ms` 或等价字段
- **AND** MUST 包含 `prepare_quant_lut_ms` 或等价字段
- **AND** MUST 让使用者判断"更快的 rotation"是否被"更大的 effective_dim 线性成本"抵消

## ADDED Requirements (from formal-baseline-study-execution)

### Requirement: Formal baseline E2E benchmark SHALL use coupled timing semantics
The `e2e-benchmark` capability SHALL support the formal baseline study's coupled `search + payload` timing contract.

#### Scenario: E2E timing starts at query submission
- **WHEN** the benchmark is run in formal baseline study mode
- **THEN** it SHALL start timing when the query vector is submitted into the system
- **AND** the measured interval SHALL include all retrieval work needed before payload fetch begins

#### Scenario: E2E timing ends at payload completion
- **WHEN** the benchmark determines the final records that must be fetched for a query
- **THEN** it SHALL trigger payload fetch within the same query path
- **AND** the E2E timer SHALL stop only after the required payload read completes

### Requirement: Formal study runs SHALL export a stable coupled-E2E metric schema
The `e2e-benchmark` capability SHALL emit a metric schema that is sufficient for the formal baseline study's comparison, aggregation, and tracking steps.

#### Scenario: Mandatory coupled-E2E fields are exported
- **WHEN** a formal-study E2E run completes
- **THEN** the benchmark output SHALL include `ann_core_ms`, `payload_fetch_ms`, `bytes_read`, `fetch_count`, `e2e_ms`, a top-k-matched recall metric for the configured `topk`, and percentile latency fields including `p99_ms`
- **AND** the parameter identity in the output SHALL be sufficient to reconstruct the originating system configuration

#### Scenario: Formal-study output location is stable
- **WHEN** a formal-study E2E run writes its per-run results
- **THEN** it SHALL write under `/home/zcq/VDB/baselines/formal-study/outputs/e2e/{dataset}/{system}/{run_id}/`
- **AND** the benchmark SHALL support generation of a dataset-level summary file for downstream aggregation

### Requirement: Formal-study E2E mode SHALL remain distinct from payload-only diagnostics
The `e2e-benchmark` capability SHALL preserve the distinction between primary coupled E2E results and payload microbench diagnostics.

#### Scenario: Payload-only measurements do not override the formal E2E result
- **WHEN** payload backend measurements are collected separately
- **THEN** they SHALL be exported to the payload microbench output family
- **AND** the benchmark SHALL NOT present those diagnostic measurements as the primary formal-study E2E result
