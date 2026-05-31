## Context

`refine-logs/formal-baseline-study-2026-04-16/` 已经定义了正式 baseline study 的方法边界、数据集矩阵、路径约定、主 baseline 套餐、联动式 E2E 协议，以及结果与 tracker 模板，但当前仓库缺少把这些文档变成实际执行流水线的统一设计。现状的问题不是缺少单个 benchmark，而是缺少一套跨数据准备、baseline 接入、正式运行和结果归档的共同 contract。

这次 change 需要跨越多个模块：

- `/home/zcq/VDB/baselines/formal-study/` 下的新脚本和结果目录
- `/home/zcq/VDB/data`、`/home/zcq/VDB/baselines/data`、`/home/zcq/VDB/third_party` 三类根路径
- Faiss / DiskANN / 可选 ConANN 与 Extended-RaBitQ 的 third-party 接入

约束条件也已经明确：

- `/home/zcq/VDB/data` 只保存原始下载数据和原始 embedding
- `/home/zcq/VDB/baselines/data` 保存 `cleaned / splits / gt / payload_*`
- 正式 E2E 必须采用单 query 路径内联动 `search + payload`
- phase-1 不改 `bench_e2e`，也不接入 BoundFetch
- phase-1 的主 IVF baseline 不再做 `nlist` sweep，而是直接固定每个数据集的主 `nlist`
- phase-1 的主 IVF baseline 不再把 `IVFPQR` 纳入正式主比较
- phase-1 的实验结构进一步拆成“主实验”与“扩展实验”
- 主实验需要覆盖 `topk = 10 / 50 / 100`
- 扩展实验只针对 `topk = 10` 做不同 payload backend 的对比
- DiskANN 是条件接入的参考线，不是 phase-1 closure 的前提
- 正式主数据集优先级固定为 `COCO 100K -> MS MARCO -> Deep8M-synth -> Amazon ESCI`

## Goals / Non-Goals

**Goals:**
- 把 formal baseline study 变成一套 implementation-ready 的分阶段实验流水线
- 固定主数据集、主 baseline、主协议、输出目录和 tracker 口径
- 让数据下载、预处理、embedding、ground truth、backend 导出和 baseline 运行共享同一套 manifest
- 让正式 E2E、payload 微基准、build/startup 成本表使用统一输出规范，便于最终汇总
- 为后续实现提供清晰的拆分边界，避免 baseline 接入和结果收集互相耦合
- 在 phase-1 中先跑通非 BoundFetch baseline family，并把 BoundFetch 保留到后续优化完成后再接入
- 冻结一版可复用的 baseline 参数选择策略，避免 smoke 参数继续混入正式 study
- 让不同 payload backend 的比较建立在相似 `recall@10` operating point 上，而不是相同原始参数上
- 把主实验和扩展实验的流程、参数和预期结果明确写死，降低后续执行歧义

**Non-Goals:**
- 不在本 change 中承诺完整跑完所有 appendix 数据集
- 不把 `search x storage x dataset` 扩成全笛卡尔积
- phase-1 不实现 BoundFetch runner，也不跑 BoundFetch 正式实验
- phase-1 不修改 `benchmarks/bench_e2e.cpp`
- 不把 DiskANN 追平或超越作为本 change 的验收前提
- 不把 payload 微基准当成正式 E2E 的替代品
- phase-1 不再对 `nlist` 做额外 sweep
- phase-1 不把 `IVFPQR` 作为正式主比较方法之一
- phase-1 不在所有 `topk` 上都做全 backend 笛卡尔积

## Decisions

### 1. 采用“manifest 驱动”的实验工作区，而不是按脚本自由组织

选择：
- 所有正式 baseline 实验都以 `/home/zcq/VDB/baselines/formal-study/` 为统一工作区
- `dataset_registry.csv`、`encoder_registry.csv`、`baseline_registry.csv`、`RUN_STATUS.csv` 和 `FAILURES.md` 先落地，再驱动后续脚本

原因：
- formal study 涉及多数据集、多系统和多输出层级，不先冻结 registry 会导致参数口径反复漂移
- manifest 是把“研究文档”转成“可执行 runbook”的最低成本方式

备选方案：
- 继续沿用零散脚本和手工 CSV
- 否决原因：难以保证不同 baseline、不同数据集之间的配置一致性

### 2. 将数据面分为 raw plane 与 formatted plane

选择：
- `/home/zcq/VDB/data` 只保存原始下载数据和原始 embedding
- `/home/zcq/VDB/baselines/data` 保存 canonical cleaned、split、ground truth 和 backend formatted data

原因：
- 这是当前文档已经冻结的路径约定
- 可以避免 raw asset 与实验产物混杂，降低重跑和清理成本

备选方案：
- 把 cleaned/gt 继续与 raw data 放在同一路径
- 否决原因：会让“原始资产”和“实验派生产物”界限再次变模糊

### 3. 将 formal baseline 拆成三条执行流水线，而不是一个大脚本

选择：
- 数据准备流水线：下载/清洗/embedding/gt/backend 导出
- baseline 执行流水线：vector search、coupled E2E、payload microbench、build/startup
- 结果汇总流水线：per-run metrics、summary CSV、plots、analysis backfill

原因：
- 三类任务的依赖、失败模式和重试粒度不同
- 这样可以先完成数据层，再逐步接入系统层，不会因为单个 baseline 卡住整个 study

备选方案：
- 用单入口脚本串起全部阶段
- 否决原因：失败恢复困难，且不利于按阶段验收

### 4. 正式 E2E 统一采用 coupled protocol，payload microbench 仅作诊断

选择：
- 正式 E2E 的时间窗口从 query 提交开始，到对应 payload 读取完成为止
- payload microbench 只用于解释 `payload_fetch_ms` 和不同 backend 的差异，不进主表替代正式 E2E

原因：
- formal baseline study 的主张是系统级 `search + payload`
- 如果用离线拼接或分头跑，会破坏与 BoundFetch 当前设计的一致性

备选方案：
- 继续用“搜索先跑完，再离线拼接 payload”的方式
- 否决原因：不符合正式主协议，也会弱化论文叙事

### 5. baseline 接入采用“统一 runner contract + system-specific adapter”

选择：
- 每个 baseline 都可以用自己的实现，但必须统一输出：
  - 搜索核 `metrics.csv`
  - 正式 E2E `metrics.csv`
  - build/startup 结果
  - run metadata
- phase-1 只接入 Faiss family 与条件性的 DiskANN；BoundFetch、ConANN、Extended-RaBitQ 先登记后置

原因：
- 可以减少对第三方实现细节的强绑定
- 统一输出后，聚合层不需要为每个系统单独写解析逻辑

备选方案：
- 强制所有 baseline 都复用同一份内部 runner
- 否决原因：第三方系统能力和输入格式差异较大，硬统一会增加集成成本

### 6. phase-1 的主 baseline 套餐收缩为 `IVFPQ + rerank`、`IVF-RQ + rerank` 与条件性的 `DiskANN`

选择：
- `Faiss IVFPQ + exact rerank` 作为主 PQ 对照
- `Faiss IVFResidualQuantizer` 或 `IVFLocalSearchQuantizer` + exact rerank 作为主 RQ 对照
- `DiskANN` 保留为条件接入的图索引参考线
- `IVFPQR` 仅保留为诊断性 runner，不进入 phase-1 主比较

原因：
- 当前 `IVFPQ baseline` 已经带 exact rerank，`IVFPQR` 再加入 residual refine 会使“量化器增强”和“最终 rerank”叠加，解释价值下降
- 收缩主 baseline 套餐后，更容易围绕 IVF family 的公平比较来组织正式结果

备选方案：
- 保留 `IVFPQR` 作为 mandatory baseline
- 否决原因：与 `IVFPQ + exact rerank` 的机制差异不足以支撑其作为独立主比较线

### 7. phase-1 参数策略固定为“固定 `nlist` + 分档 `topk` + 仅 sweep `nprobe`”

选择：
- 主实验固定 `topk ∈ {10, 50, 100}`
- `candidate_budget` 与 `topk` 绑定：
  - `topk=10 -> candidate_budget=100`
  - `topk=50 -> candidate_budget=250`
  - `topk=100 -> candidate_budget=500`
- 每个主数据集固定一档主 `nlist`
  - `COCO 100K = 2048`
  - `Deep8M-synth = 12800`
  - `MS MARCO Passage = 16384`
  - `Amazon ESCI = 8192`，若清洗后规模明显偏离，再做一次显式调整
- `IVFPQ + rerank` 与 `IVF-RQ + rerank` 第一轮只 sweep `nprobe`
- `DiskANN` 若接入，仅 sweep `L_search`

原因：
- 当前 formal-study smoke 参数明显偏低，继续保留 `nlist` sweep 只会把正式实验空间拉得过大
- `topk` 现在不再只取 20，而是用于刻画结果规模扩展趋势，因此 rerank budget 也必须按 `topk` 分档，而不是继续使用单一常数
- 你本地旧结果已经给出了可用的 `nlist` operating points，重新做全量 `nlist` sweep 收益低
- Faiss 的 rerank 口径本质上是 `k_factor * topk`；将 `candidate_budget` 和 `topk` 绑定可以保持不同 `topk` 之间的 rerank 强度更可比

备选方案：
- 同时 sweep `nlist`、`nprobe` 和 `candidate_budget`
- 否决原因：会显著放大实验矩阵，而且更难判断 latency/recall 变化到底来自 coarse partition、probe 深度还是 rerank budget

### 8. 正式实验拆成“主实验”与“扩展实验”

选择：
- 主实验只在主 backend `FlatStor` 上运行
- 主实验覆盖 `topk=10 / 50 / 100`
- 扩展实验只对 `topk=10` 做不同 backend 的比较
- appendix 数据集保留在扩展实验之后继续门控

原因：
- `topk=10 / 50 / 100` 回答的是“结果集规模变化时搜索核和 E2E 如何扩展”
- backend 对比回答的是“在真实 serving 口径下，不同 payload backend 的差异有多大”
- 把两类问题拆开后，主结果图和扩展图的叙事更清楚

备选方案：
- 对每个 `topk` 都完整跑 `FlatStor / Lance / Parquet`
- 否决原因：矩阵膨胀明显，而且 backend 结论容易被大 `topk` 的 fetch 规模主导

### 8.1 主实验执行粒度进一步按数据集拆分

选择：
- `8.7` 之后的执行任务不再以“整套主实验”作为最小单元
- 改为以 `dataset` 为一级验收边界：
  - `COCO 100K`
  - `MS MARCO Passage`
  - `Deep8M-synth`
  - `Amazon ESCI`
- 每个数据集内部再拆成：
  - gate run
  - `topk=10`
  - `topk=50`
  - `topk=100`
  - dataset-level summary / short report

原因：
- 当前 `8.7` 过大，任一数据集失败会阻塞整批实验
- 数据集级拆分后，已完成数据集可以先单独验收、单独画图、单独写结论
- 这与当前 `coco_100k` 已经单独形成结果的现状一致

备选方案：
- 继续保持“整套主实验一次跑完”
- 否决原因：失败恢复粒度太粗，且任务状态无法准确反映局部完成成果

### 8.2 每个主数据集在 sweep 前增加 gate run

选择：
- 每个主数据集在正式 sweep 前必须先通过一组单点 gate：
  - 单点 vector-search
  - 单点 coupled E2E
  - metric schema 检查
  - top ids / gt / labels / payload alignment 检查
- 只有 gate 通过后，才允许进入该数据集的全量 `nprobe` 或 `L_search` sweep

原因：
- 现有 `msmarco_passage` 已暴露出 query/gt/index alignment 问题
- 单点 gate 可以把“runner bug”和“长时间 sweep”解耦，避免数小时后才发现基本契约错误

备选方案：
- 数据准备完成后直接进入 sweep
- 否决原因：错误发现过晚，排障成本高，而且浪费运行时间

### 9. 扩展实验的 storage backend 比较采用 recall-matched operating points，而不是原始参数对齐

选择：
- 先在主实验 `topk=10` 的结果上完成 search-side sweep
- 对每个 `dataset × method`，按 `recall@10` 目标值选择 operating point
  - 目标值固定为 `0.80`、`0.90`、`0.95`
- 再用这些 recall 对齐的点去跑 `FlatStor / Lance / Parquet`

原因：
- backend 对比的目标是观察相似检索质量下的端到端开销差异，而不是比较同一组 `nprobe` 在不同系统上的含义
- 对 `topk=10` 使用 `recall@10` 选点，比沿用 `recall@20` 更符合 serving 场景的结果质量定义

备选方案：
- 直接在不同 backend 上复用同一组 `nprobe`
- 否决原因：相同 `nprobe` 不代表相同 recall 水平，结果不公平
### 10. 主数据集与 appendix 数据集分阶段门控

选择：
- 先完成四个主数据集：`COCO 100K`、`MS MARCO Passage`、`Deep8M-synth`、`Amazon ESCI`
- `LAION subset / Clotho / MSR-VTT` 只在主数据集工作流稳定后再进入执行

原因：
- formal study 的当前目标是建立最小可发表 baseline 套餐，不是一次性铺满所有模态
- 主数据集已经足以覆盖图文、文本、商品检索和 payload size effect 四类核心场景

备选方案：
- 同步推进主表和 appendix 数据集
- 否决原因：会明显稀释实现和调试资源

### 11. 结果汇总先落 formal-study 工作区，再回填公共 results

选择：
- 所有原始运行结果、summary CSV、tracker 和 plots 先落到 `/home/zcq/VDB/baselines/formal-study/`
- 只有在结果通过检查后，才回填到 `/home/zcq/VDB/baselines/results/` 和 narrative analysis

原因：
- formal-study 是本轮实验的专用工作区
- 这样可以避免半成品结果直接污染公共 `results/` 目录

备选方案：
- 一开始就直接写公共 results
- 否决原因：中间失败或重跑会导致结果目录难以维护

## Experiment Packages

### Main Experiments

目标：
- 在统一主 backend 下观察不同方法在 `topk=10 / 50 / 100` 下的搜索与 E2E 扩展趋势

流程：
1. 冻结数据集、encoder、baseline registry
2. 为主数据集补齐 `gt_top100.npy`
3. 按数据集顺序执行：
   - `COCO 100K`
   - `MS MARCO Passage`
   - `Deep8M-synth`
   - `Amazon ESCI`
4. 每个数据集内部先做 gate：
   - 单点 vector-search
   - 单点 coupled E2E
   - metric / id / gt / payload alignment 检查
5. gate 通过后，再对该数据集按 `topk=10 / 50 / 100` 展开主 sweep：
   - 固定 `nlist`
   - 固定主 backend `FlatStor`
   - 固定 `candidate_budget`
   - sweep `nprobe` 或 `L_search`
6. 对该数据集先跑 vector-search，再跑 coupled E2E
7. 每个数据集完成后立即生成 dataset-level summary、Pareto 图和 short report
8. 四个主数据集都完成后，再汇总生成 main-suite summary 与 plots

参数：
- 数据集：
  - `COCO 100K`
  - `MS MARCO Passage`
  - `Deep8M-synth`
  - `Amazon ESCI`
- 方法：
  - `Faiss IVFPQ + rerank`
  - `Faiss IVF-RQ + rerank`
  - `DiskANN` 可选参考
- `topk ∈ {10, 50, 100}`
- `candidate_budget ∈ {100, 250, 500}`，与 `topk` 绑定
- 固定 `nlist`：
  - `COCO=2048`
  - `Deep8M=12800`
  - `MS MARCO=16384`
  - `Amazon ESCI=8192`
- sweep：
  - IVF family: `nprobe`
  - DiskANN: `L_search`

预期结果：
- `topk` 增大时，`ann_core_ms`、`payload_fetch_ms`、`e2e_ms` 整体单调上升
- `topk=10` 将成为后续 backend 扩展实验的 serving anchor
- `topk=50/100` 将提供结果规模扩展趋势，而不是替代 top-10 serving 口径
- 单个主数据集失败不应阻塞其它数据集已经完成结果的归档与分析

### Extended Experiments

目标：
- 在真实 serving 口径 `topk=10` 下，隔离不同 payload backend 对端到端性能的影响

流程：
1. 使用各主数据集 `topk=10` 的 search sweep 结果
2. 对每个 `dataset × method` 选择最接近目标 recall 的点：
   - `recall@10 ≈ 0.80`
   - `recall@10 ≈ 0.90`
   - `recall@10 ≈ 0.95`
3. 先按数据集写 operating-point manifest
4. 再按数据集重放 `FlatStor / Lance / Parquet`
5. 每个数据集完成后立即生成 backend comparison summary
6. 所有主数据集 backend replay 完成后，再汇总 plot-ready CSV 和 narrative analysis
7. 只有主实验和 backend 扩展稳定后，才继续 appendix 数据集

参数：
- 固定 `topk=10`
- 固定 `candidate_budget=100`
- operating point 选择指标：`recall@10`
- backend：
  - `FlatStor`
  - `Lance`
  - `Parquet`

预期结果：
- backend 差异主要体现在 `payload_fetch_ms`、`e2e_ms` 和 `p99_ms`
- search-side 排序在同一 operating point 下应保持稳定，backend 变化不应改变搜索质量定义
- 若某方法达不到 `recall@10=0.95`，将以 `unreached` 或 `best-effort` 明确记录

## Risks / Trade-offs

- [风险] 第三方 baseline 的安装和构建成本高于仓库内 BoundFetch
  → Mitigation：先冻结 `baseline_registry.csv` 和 `/home/zcq/VDB/third_party` 目录结构，再按优先级接入 `Faiss -> DiskANN -> optional baselines`

- [风险] 多数据集的数据准备时间长，可能阻塞 benchmark 接入
  → Mitigation：按照 `COCO -> MS MARCO -> Deep8M-synth -> ESCI` 的固定顺序分批推进，保证每批都可独立验收

- [风险] 各 baseline 的输入格式差异导致 E2E 结果难以统一
  → Mitigation：统一 runner contract，只要求 adapter 产出标准化 metrics，而不强行统一内部实现

- [风险] payload microbench 被误用为正式 E2E 结论
  → Mitigation：在 spec 和任务里显式区分 `outputs/payload_microbench` 与 `outputs/e2e`

- [风险] `Amazon ESCI` 的清洗后规模可能让 `nlist=8192` 不再是合适固定点
  → Mitigation：只允许在数据清洗完成后做一次显式 cardinality review，确认是否保留 `8192`

- [风险] 某些 baseline 在个别数据集上达不到扩展实验所需的 `recall@10 = 0.95`
  → Mitigation：聚合阶段把这类点显式记录为 `unreached` 或 `best-effort`，不强行凑齐三档 recall

- [风险] `topk=50/100` 主实验若没有 `gt_top100.npy` 会导致评估契约不完整
  → Mitigation：把 `gt_top100.npy` 明确纳入主实验前置条件，并在 registry/validation 中显式检查

- [风险] formal-study 输出与公共 results 双写，可能产生版本漂移
  → Mitigation：规定先写 formal-study，后做受控回填；回填必须由聚合步骤触发

## Migration Plan

1. 初始化 `formal-baseline-study-execution` change 下的 spec contract，冻结数据、baseline、输出和 tracker 要求。
2. 在 `/home/zcq/VDB/baselines/formal-study/` 下建立 manifests、scripts、outputs、trackers 的目录骨架。
3. 优先实现主数据集的数据准备脚本和 manifest 回填，再接入主 baseline 套餐。
4. 在 `formal-study/scripts/run_e2e_coupled/` 下实现 coupled E2E runner，使其满足统一字段要求。
5. 完成 per-run metrics、summary CSV、build/startup 表和图表脚本。
6. 结果稳定后，再回填 `baselines/results/` 与 narrative analysis 文档。

回滚策略：

- 若某个 baseline 或数据集未准备完成，可以只冻结 manifest 状态为 `planned`/`blocked`，不影响已完成批次继续运行。
- 若 formal-study 输出结构需要调整，只回滚 `/home/zcq/VDB/baselines/formal-study/` 的工作区，不修改 raw data 根目录。
- 若 coupled E2E 集成未完成，payload microbench 仍可保留为诊断项，但不得提升为主结果。

## Open Questions

- `Faiss IVFResidualQuantizer` 与 `IVFLocalSearchQuantizer` 具体选哪一个作为正式主 baseline，仍要以实际集成稳定性决定。
- `ConANN + Faiss` 是否在本轮 change 内实现，还是保留在可选扩展项，取决于第三方构建成本。
- `Amazon ESCI` 的清洗后商品数若与当前预估差异较大，是否需要把固定 `nlist` 从 `8192` 调整到相邻档位，仍需在数据落地后确认。
