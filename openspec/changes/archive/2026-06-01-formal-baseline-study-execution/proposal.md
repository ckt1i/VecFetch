## Why

`refine-logs/formal-baseline-study-2026-04-16/` 已经把正式 baseline study 的数据集、baseline 套餐、联动式 E2E 协议、路径口径和记录模板定义清楚，但这些内容还停留在研究计划层，没有对应的 implementation-ready OpenSpec change。当前仓库仍缺少一套统一的执行方案，把数据下载与预处理、非 BoundFetch baseline 接入、正式实验运行和结果收集串成同一条可落地流水线。

## What Changes

- 新增一套 `formal baseline study` 执行能力，把 `formal-baseline-study-2026-04-16` 下的方案收敛成可实现、可追踪、可复跑的实验 change。
- 新增主数据集准备流程，覆盖：
  - `COCO 100K`
  - `MS MARCO Passage`
  - `Deep8M + synthetic payload`
  - `Amazon ESCI`
  - 以及 appendix 级的 `LAION subset / Clotho / MSR-VTT`
- 新增 manifest-driven 数据准备要求，明确：
  - `/home/zcq/VDB/data` 只保存原始下载数据与原始 embedding
  - `/home/zcq/VDB/baselines/data` 保存 `cleaned / splits / gt / payload_*`
  - `/home/zcq/VDB/baselines` 保存运行结果、图表和 tracker
  - `/home/zcq/VDB/third_party` 保存手动下载或编译的第三方库
- 新增 phase-1 正式 baseline 套餐执行要求，覆盖：
  - Faiss `IVFPQ + rerank`
  - Faiss `IVFResidualQuantizer` 或 `IVFLocalSearchQuantizer`
  - DiskANN 薄封装 smoke gate
  - `IVFPQR` 仅保留为诊断性 runner，不进入 phase-1 主比较
  - 可选的 `ConANN + Faiss` 与 `Extended-RaBitQ` 仅登记，不阻塞 phase 1
- 明确 phase-1 暂不实现：
  - BoundFetch runner
  - `benchmarks/bench_e2e.cpp` 改造
  - BoundFetch 正式实验
- 将 phase-1 拆成主实验和扩展实验两层：
  - 主实验：在主数据集上固定 `nlist`、固定主后端 `FlatStor`，运行 `topk ∈ {10, 50, 100}` 的主搜索与 coupled E2E
  - 扩展实验：仅对 `topk=10`，按 `recall@10` 选出 operating points，再重放到 `FlatStor / Lance / Parquet`
- 冻结主实验的 baseline 参数策略，明确：
  - 主实验 `topk ∈ {10, 50, 100}`
  - `candidate_budget` 与 `topk` 绑定：`{100, 250, 500}`
  - IVF family 第一轮仅 sweep `nprobe`
  - `nlist` 直接固定为每个主数据集的一组既有/指定 operating point，而不是再做全量 sweep
  - 为 `topk=100` 主实验补齐 `gt_top100.npy`
- 新增扩展实验的 recall-matched backend 比较要求，明确：
  - 先在主实验的 `topk=10` 结果上完成 search-side sweep
  - 再按 `recall@10 ≈ 0.80 / 0.90 / 0.95` 选出 operating points
  - 仅用这些 recall 对齐的点去跑 `FlatStor / Lance / Parquet`
- 新增主实验和扩展实验各自的预期结果定义，明确：
  - 主实验用于观察 `topk=10/50/100` 下搜索核与 E2E 的扩展趋势
  - 扩展实验用于分离 backend 对 `payload_fetch_ms / e2e_ms / p99_ms` 的影响
- 将实验执行粒度进一步细化，明确：
  - 不再把整套主实验压成单个长任务
  - 主实验按 `dataset -> gate -> method -> topk -> summarize` 分层拆开
  - 扩展实验按 `dataset -> operating point selection -> backend replay -> summarize` 分层拆开
  - 允许每个主数据集单独完成、单独出小报告、单独进入后续 backend 扩展
- 新增正式实验运行矩阵与顺序，覆盖：
  - 搜索核 benchmark
  - 联动式 `search + payload` E2E benchmark
  - payload backend 微基准
  - build/startup 成本表
- 修改现有 `e2e-benchmark` 要求，使其支持 formal baseline study 所需的主协议和输出字段：
  - 计时从 query 提交开始，到原始数据读取完成结束
  - 输出统一的 `ann_core_ms / payload_fetch_ms / e2e_ms / p99_ms / bytes_read / fetch_count` 与 top-k-matched recall 指标
  - phase-1 由 `/home/zcq/VDB/baselines/formal-study/scripts/run_e2e_coupled/` 提供该协议，不要求改动 `bench_e2e`
- 新增结果收集与记录要求，明确：
  - manifests
  - `RUN_STATUS.csv`
  - `FAILURES.md`
  - per-run `metrics.csv`
  - 主实验 summary 与扩展实验 summary
  - 统一图表与 narrative analysis 更新目标

## Capabilities

### New Capabilities
- `formal-baseline-data-prep`: 定义正式 baseline study 的数据下载、清洗、embedding 生成、ground truth 生成、payload backend 导出和路径约定
- `formal-baseline-execution`: 定义正式 baseline 套餐、主实验/扩展实验分层、联动式 E2E 协议、payload 微基准、build/startup 评估和执行顺序
- `formal-baseline-result-tracking`: 定义 manifests、tracker、per-run 输出、按 `recall@10` 选点的扩展实验聚合结果、图表与分析文档更新要求

### Modified Capabilities
- `e2e-benchmark`: 将端到端 benchmark 从历史的松散导出升级为 formal baseline study 所需的联动式 `search + payload` 主协议与统一输出口径

## Impact

- Affected docs:
  - `refine-logs/formal-baseline-study-2026-04-16/*.md`
  - `refine-logs/formal-baseline-study-2026-04-16/*.csv`
- Affected code and scripts:
  - `/home/zcq/VDB/baselines/formal-study/scripts/prepare_datasets/*`
  - `/home/zcq/VDB/baselines/formal-study/scripts/build_embeddings/*`
  - `/home/zcq/VDB/baselines/formal-study/scripts/build_groundtruth/*`
  - `/home/zcq/VDB/baselines/formal-study/scripts/run_vector_search/*`
  - `/home/zcq/VDB/baselines/formal-study/scripts/run_e2e_coupled/*`
  - `/home/zcq/VDB/baselines/formal-study/scripts/export_payload_backends/*`
- Affected outputs and trackers:
  - `/home/zcq/VDB/baselines/formal-study/manifests/*`
  - `/home/zcq/VDB/baselines/formal-study/outputs/*`
  - `/home/zcq/VDB/baselines/formal-study/trackers/*`
  - `/home/zcq/VDB/baselines/results/*`（正式汇总阶段）
- Affected dependencies:
  - `/home/zcq/VDB/third_party/faiss`
  - `/home/zcq/VDB/third_party/diskann`
  - `/home/zcq/VDB/third_party/conann`（registry only in phase 1）
  - `/home/zcq/VDB/third_party/extended-rabitq`（registry only in phase 1）
