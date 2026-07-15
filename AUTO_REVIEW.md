# Auto Review Loop: Budgeted Prefetch Pipeline

## Round 1 (2026-07-03T12:52:11+08:00)

### Scope

本轮目标是在保持最终 rerank budget 上限的前提下，验证更激进 probe/prefetch 是否能提升性能：

- 先跑 no-budget 上界测试，验证“当前 budget 推迟 I/O 导致 pipeline 收益受限”的判断。
- 再实现并测试方案B：保留最终 top-B rerank budget，增加 speculative raw-vector prefetch cache。
- 先在 Amazon ESCI 上做参数探索，再扩展到五个数据集。
- 最后与 no-pipeline 方案对比。

### Initial Code Finding

当前实现中，开启 `non_safeout_candidate_budget > 0` 后，SafeIn/Uncertain 产生的 read plan 会先进入 `budgeted_read_plan_heap_`；所有 cluster probe 结束后才在 `MaterializeBudgetedReadPlans()` 中转成真正的 I/O 请求。因此 budget 口径下的执行路径不是完整的 probe/read/rerank overlap。

### Reviewer Setup

本会话没有暴露 `mcp__codex__codex` / `mcp__codex__codex-reply` 工具。按照 auto-review-loop 的意图，本轮改用可用的多代理 reviewer 做独立审查，并将结果记录在本文件。

### Actions Started

- 创建实验目录：`/home/zcq/VDB/test/recordgate_budgeted_prefetch_pipeline_20260703`
- 准备启动独立 reviewer。
- 准备复用现有索引跑 Amazon no-budget 上界测试。

### Reviewer Feedback Summary

独立 reviewer 指出：

- no-budget 只能作为上界，因为它同时改变了读量、rerank 数和 payload 行为。
- online runner 需要输出 `candidate_budget_*`、`io_wait` 和方案B专项指标，才能支持后续判断。
- 方案B保持最终 rerank budget 的 invariant 基本正确，但投机上限会被早期后续淘汰的候选消耗。
- 初始 sweep 的小幅收益需要警惕顺序和 OS page cache 干扰。

### Actions Taken

- 实现 `--budgeted-prefetch-limit`。
- 实现 budget-preserved speculative raw-vector prefetch cache。
- 实现 cache hit owning-buffer 转交，减少二次 copy。
- 补充 online runner 诊断输出：candidate budget、unique fetch、io wait、prefetch 专项统计。
- 跑完 Amazon no-budget 上界、Amazon-only sweep、五数据集 sweep、warm paired repeat、selected pipeline vs no-pipeline 对比。

### Results

本轮完整结果见：

- `/home/zcq/VDB/test/recordgate_budgeted_prefetch_pipeline_20260703/RESULT_SUMMARY_CN.md`

核心结论：方案B保持 recall 和最终 rerank budget，但不是通用加速。它对 ImageNet1K/VoxCeleb2 的部分 high-drain workload 有收益，其中 VoxCeleb2 top100 在 selected pipeline 对比中约快 34.5%；对 Amazon/MSMARCO/COCO 建议保持关闭。

### Status

Round 1 已完成实验闭环；外部 MCP reviewer 不可用，已用多代理 reviewer 替代。

---

# Auto Review Loop: Adaptive Hot/Cold Record Store

## Round 1 (2026-07-08T18:30:00+08:00)

### Scope

本轮目标是验证“大 payload 不应继续和 raw vector 紧耦合放在同一个 combined record 中”的格式方案：

- 复用现有 RecordGate/RaBitQ 索引，不重建向量索引。
- 新增 hot/cold record sidecar：`hotvec.dat` 保存按 record 排列的原始向量，`payload.cold.dat` 保存完整 payload，`hotcold_map.bin` 保存 record 到两者偏移的映射。
- 查询阶段通过 `--hotcold-store-dir` 使用新 sidecar，并保持 combined-store 与已有 no-combine separate-store 行为不变。
- 在 VoxCeleb2 和 MSMARCO 上对比 combined、existing separate-store 与 hot/cold。

### Reviewer Setup

本会话仍未暴露 `mcp__codex__codex` / `mcp__codex__codex-reply` 工具。为满足独立审查流程，尝试使用 `codex exec` 调用外部 reviewer；该命令启动了 `gpt-5.5 xhigh`，但环境中的 shell sandbox 失败：

```text
bwrap: loopback: Failed RTM_NEWADDR: Operation not permitted
```

因此外部 reviewer 未能读取工作区文件或实验 CSV，只能基于不完整上下文给出“不可验证”的低分反馈。随后尝试增量刷新 code-review-graph，但图构建在 300s 后超时。因此，本轮有效审查以本文件中的可复现实证为准，并明确保留“外部自动 reviewer 未完成实质代码审查”的风险。

### Implementation Evidence

- 新增 hot/cold materializer 目标：`bench_materialize_hotcold_store`。
- 查询 runner 新增参数：`--hotcold-store-dir`，并在 JSON 中输出 `record_layout=hotcold_record_store`。
- `bench_online_query` 对 combined、no-combine separate-store、hot/cold 三种 record layout 做互斥选择，避免混用。
- `OverlapScheduler` 复用已有 sidecar fd 路径，使 hot/cold 模式下 raw vector 读 `hotvec.dat`，payload prefix/suffix 读 `payload.cold.dat`。
- 新增测试 `PayloadPipelineTest.HotColdStoreSafeInPrefixThresholdFetchesSuffix`，验证 hot/cold sidecar 下 SafeIn 前缀读取与后续补读可以重建完整 payload。

### Verification

已完成以下本地验证：

- `cmake --build build --target bench_materialize_hotcold_store test_payload_pipeline -- -j2`
- `./build/test_payload_pipeline`
- `git diff --check`

`test_payload_pipeline` 共 7 个测试通过，其中包括新增 hot/cold prefix/suffix 测试。

### Experiment Evidence

结果目录：

- `/home/zcq/VDB/test/recordgate_hotcold_record_store_20260708`

物化后的 hot/cold store：

- `/home/zcq/VDB/test/data/voxceleb2_ecapa_150k/indexes/recordgate/hotcold_record_store_v1`
- `/home/zcq/VDB/test/data/msmarco_passage/indexes/recordgate/hotcold_record_store_v1`

实验设置：

- 数据集：VoxCeleb2 ECAPA 150K、MSMARCO Passage。
- 查询：topk=10，nprobe=64，queries=1000，repeats=3。
- Record layout：combined、existing separate-store、hotcold_record_store。
- SafeIn threshold：16 KiB 与 256 KiB，按 QPS 选择各 layout 最优点。
- `two-level-coarse-routing=1`，`two-level-coarse-budget-factor=16`。
- `non-safeout-candidate-budget=0`。

核心结果：

| dataset | comparison | QPS delta | latency delta | recall delta |
|---|---:|---:|---:|
| VoxCeleb2 | hot/cold vs combined | +13.72% | -12.06% | 0 |
| VoxCeleb2 | hot/cold vs separate | +2.78% | -2.70% | 0 |
| MSMARCO | hot/cold vs combined | +6.88% | -6.44% | 0 |
| MSMARCO | hot/cold vs separate | -0.28% | +0.29% | 0 |

完整结果见：

- `/home/zcq/VDB/test/recordgate_hotcold_record_store_20260708/RESULT_SUMMARY_CN.md`
- `/home/zcq/VDB/test/recordgate_hotcold_record_store_20260708/results/hotcold_layout_best_comparison.csv`

### Review Findings

- 正向结论成立：hot/cold 在两个目标数据集上均优于 combined-store，证明“大 payload 与 raw vector 解耦”能降低 combined data.dat 随机小读成本。
- VoxCeleb2 进一步超过 existing separate-store，说明对于大 payload、较小 raw vector 的负载，新布局有机会成为默认格式。
- MSMARCO 只与 separate-store 基本持平且略慢，不能 claim hot/cold 普遍优于 separate-store。该数据集的 raw-vector plane 很大，额外 map lookup/fd 路径成本会抵消部分收益。
- 当前实现仍是 sidecar 版本，不是最终 cluster trailer pointer 版本；论文中若描述为格式创新，需要区分“已验证的物理分离效果”和“后续可集成到正式 cluster 文件格式”的工程形态。

### Next Steps

- 若继续优化 MSMARCO，应优先做 payload/read coalescing、map lookup 缓存、以及把 hot/cold pointer 写入 cluster trailer，减少 sidecar map 访问。
- 若技术冻结，建议将 claim 写成：hot/cold layout 相比 combined-store 在大 payload 数据集上稳定降低访问成本；与 separate-store 的胜负依赖 workload，但 VoxCeleb2 已出现正向结果。

### Status

Round 1 已完成实现、测试、物化和双数据集实验。外部自动 reviewer 因 sandbox 与 graph 刷新问题未完成实质审查，本轮保留该风险并以本地可复现实证作为完成依据。

## Round 2 (2026-07-09T01:52:55+08:00)

### Scope

本轮目标是完成 Phase 2 inline hot-record store，并验证用户提出的阈值合并语义：

- `inline_payload_threshold` 同时决定 payload 是否随 raw vector 内联，以及 SafeIn 是否能提前获得 payload。
- cold payload 不再使用独立 `safein_prefetch_threshold` 做 SafeIn 前缀读，而是在 final top-k 后通过 descriptor 指向 `payload.cold.dat` 补读。
- derived index 的 `cluster.clu` 地址直接指向 hot record，查询期不加载 `hotcold_map.bin` 或 `address_map.bin`。

### Reviewer Setup

本会话没有暴露 `mcp__codex__codex` / `mcp__codex__codex-reply` 工具，因此无法执行 auto-review-loop 中的外部 GPT MCP 审查。已使用 `code-review-graph` 对当前变更做结构风险扫描：

- 变更文件：16 个。
- 影响函数/类：59 个。
- 受影响 flow：13 个。
- 风险分：0.85。
- 主要风险集中在 query benchmark 主入口、参数解析、`OverlapScheduler` 和 `RerankConsumer`。

因此本轮以可复现测试、实验结果和人工自审作为审查依据，并明确保留“缺少外部 reviewer 独立复查”的风险。

### Implementation Evidence

- 新增 `include/vdb/storage/hot_record.h`：
  - `HotPayloadDescriptor`
  - `HotPayloadStorageType::{inline_payload,cold_pointer,prefix_cold_pointer}`
  - descriptor 编解码和校验；当前明确拒绝 reserved `prefix_cold_pointer`。
- 新增 `bench_materialize_inline_hot_record_store`：
  - 从既有 combined RecordGate index 物化派生索引。
  - 重写 `cluster.clu` 中的 `AddressEntry.offset/size`，使其直接指向派生 `data.dat` 中的 hot record。
  - 生成 `payload.cold.dat` 和 manifest；manifest 报告 `layout=inline_hot_record_store`、descriptor bytes、inline threshold、effective SafeIn inline threshold、inline/cold record counts、hot/cold bytes、`address_map_bytes=0`。
- 更新 query 路径：
  - `bench_e2e`/`bench_online_query` 支持 `--inline-hot-record-store-dir`。
  - inline 模式打开派生 `data.dat` 和可选 `payload.cold.dat`，不加载 sidecar map。
  - exact rerank 读取 `raw_vector + descriptor`，只把 raw vector 喂给 rerank。
  - SafeIn 对 inline payload 可缓存 payload；对 cold pointer 只缓存/使用 descriptor，cold payload 延迟到 final top-k。
  - JSON 输出 `record_layout=inline_hot_record_store`、`inline_sidecar_map_records=0`、`inline_sidecar_map_bytes=0`、descriptor/cold/cache 诊断。

### Verification

已完成以下本地验证：

- `git diff --check` 通过。
- `cmake --build build --target bench_e2e -j$(nproc)` 通过。
- `cmake --build build --target bench_materialize_inline_hot_record_store -j$(nproc)` 通过。
- `cmake --build build --target test_payload_pipeline test_rerank_consumer test_overlap_scheduler -j$(nproc)` 通过。
- `ctest --test-dir build --output-on-failure -R 'test_(payload_pipeline|rerank_consumer|overlap_scheduler)$'` 通过。
- COCO threshold=0 smoke query 成功，JSON 报告 `record_layout=inline_hot_record_store`、sidecar map records/bytes 为 0、descriptor error 为 0。

### Experiment Evidence

结果目录：

- `/home/zcq/VDB/test/recordgate_inline_hot_record_store_20260709`

派生索引目录：

- `/home/zcq/VDB/test/data/coco_100k/indexes/recordgate/inline_hot_record_store_thr*`
- `/home/zcq/VDB/test/data/amazon_esci/indexes/recordgate/inline_hot_record_store_thr*`
- `/home/zcq/VDB/test/data/msmarco_passage/indexes/recordgate/inline_hot_record_store_thr*`
- `/home/zcq/VDB/test/data/voxceleb2_ecapa_150k/indexes/recordgate/inline_hot_record_store_thr*`

实验覆盖：

- 数据集：COCO100K、Amazon ESCI、MSMARCO Passage、VoxCeleb2 ECAPA 150K；不包含 ImageNet。
- 查询：`nprobe=64`，`topk=10/100`，`queries=1000`，two-level coarse routing enabled，`budget_factor=16`，`non_safeout_candidate_budget=0`。
- 结果数：64 个 `results.json`。
- 诊断检查：64 行正式结果中 `inline_sidecar_map_records=0`、`inline_sidecar_map_bytes=0`、`avg_inline_descriptor_errors=0`。

核心结果见：

- `/home/zcq/VDB/test/recordgate_inline_hot_record_store_20260709/RESULT_SUMMARY_CN.md`
- `/home/zcq/VDB/test/recordgate_inline_hot_record_store_20260709/ANALYSIS_CN.md`
- `/home/zcq/VDB/test/recordgate_inline_hot_record_store_20260709/results/inline_hot_record_store_raw.csv`
- `/home/zcq/VDB/test/recordgate_inline_hot_record_store_20260709/results/inline_hot_record_store_comparison.csv`

### Results and Claim Boundaries

- 阈值合并合理且已实现：inline 模式下 `inline_payload_threshold` 是唯一的 payload placement/SafeIn payload 上限；cold payload 不做 SafeIn 前缀读。
- 格式 claim 成立：cluster address 可以直接定位 hot record，查询期不需要 sidecar map。
- 性能 claim 必须收窄：
  - VoxCeleb2 topk=100 上 inline eager 最强，`thr=512KB` 相比 inline late 约 +29.9% QPS，且强于同轮 combined/no-combine 对照。
  - VoxCeleb2 topk=10 主要强于 no-combine 和 late，但不稳定超过 combined。
  - COCO、Amazon ESCI、MSMARCO 上 inline eager 通常慢于 combined 或 inline late，不能 claim 通用加速。
  - 与 Phase 1 sidecar hot/cold 的 topk=10 结果相比，Phase 2 inline 当前没有证明“去掉 map 后更快”；descriptor 重读和 hot-record 小读成本抵消了 map 移除收益。

### Weaknesses

- 当前实现会在 SafeIn/consume 和 final materialization 中重复解析部分 descriptor；topk=10 下常见 `avg_inline_descriptor_read_requests=20`，仍有优化空间。
- Phase 2 inline 对 MSMARCO 的 eager SafeIn 明显弱于 late，说明 SafeIn 策略需要 workload gating。
- Phase 1 sidecar 与 Phase 2 inline 的 topk=100 对比没有完整重跑；已有结论主要依赖 topk=10 Phase 1 结果和 Phase 2 topk=10/100 结果。
- 外部 auto reviewer 未完成实质复查。

### Status

Round 2 完成实现、测试、四数据集实验和结果分析。结论为：实现足以支持“direct-address payload-aware record format + threshold-merged SafeIn”这一受限 claim；不支持“inline format 在所有数据集/所有布局上普遍加速”的强 claim。

## Method Description

Phase 2 inline hot-record store 将 RecordGate 的 cluster address 直接映射到记录存储。派生索引中的 `cluster.clu` 保存指向新 `data.dat` hot record 的 `AddressEntry`；每条 hot record 以原始向量开头，随后是固定大小 `HotPayloadDescriptor`。小 payload 直接内联在 hot record 中，大 payload 存入 `payload.cold.dat` 并由 descriptor 指向其 offset 和 length。

查询时，exact rerank 读取 `raw_vector + descriptor`，只用 raw vector 计算精确距离。SafeIn 不再依赖独立 payload prefetch threshold：如果 descriptor 表示 inline payload，则可随 hot record 一并缓存 payload；如果 descriptor 表示 cold pointer，则 SafeIn 只获得原始向量和元数据，完整 payload 在 final top-k 后再从 cold 文件补读。
