# Spec: Query Pipeline

## Feature: AsyncReader

### Scenario: PreadFallbackReader 同步读取
- Given 一个已打开的 DataFile（fd 有效）
- When 提交一个 IoRequest{fd, offset=0, length=512, buffer}
- Then Submit 返回 1（成功提交 1 个）
- And Poll 返回 1 个 IoCompletion{result=512}
- And buffer 中包含文件偏移 0 处的 512 字节数据
- And InFlight() 返回 0（同步完成）

### Scenario: PreadFallbackReader 批量提交
- Given 一个包含 100 条记录的 DataFile
- When 提交 10 个 IoRequest（不同 offset）
- Then Submit 返回 10
- And Poll 返回 10 个 IoCompletion，result 均为正数
- And 每个 buffer 包含对应 offset 的正确数据

### Scenario: PreadFallbackReader 读取超出文件范围
- Given 一个大小为 1000 字节的文件
- When 提交 IoRequest{offset=900, length=200}
- Then IoCompletion.result = 100（只读到文件末尾）

### Scenario: IoUringReader 基本读写（需 liburing）
- Given io_uring 可用（内核 >= 5.1）
- And 一个已打开的 DataFile
- When Init(queue_depth=32)
- And Submit 一个 IoRequest
- And WaitAndPoll
- Then 返回 1 个 IoCompletion{result > 0}
- And buffer 包含正确数据

### Scenario: IoUringReader SQ 满时部分提交
- Given Init(queue_depth=4)
- And 已有 4 个 in-flight 请求
- When Submit 2 个新请求
- Then Submit 返回 0（SQ 满）
- And InFlight() == 4

---

## Feature: ReadQueue

### Scenario: Qv 优先出队
- Given 空队列（max_capacity=256）
- When Push 3 个 priority=1 (Qp) 的 task
- And Push 2 个 priority=0 (Qv) 的 task
- And PopBatch(max_count=5)
- Then 前 2 个是 Qv task，后 3 个是 Qp task

### Scenario: 队列容量上限 back-pressure
- Given max_capacity=4 的队列
- When Push 4 个 task → 均返回 true
- And Push 第 5 个 task
- Then 返回 false（队列满）
- And Size() == 4

### Scenario: 空队列 PopBatch
- Given 空队列
- When PopBatch(max_count=10)
- Then 返回 0

### Scenario: Qv 全取完后取 Qp
- Given 队列中有 1 个 Qv task + 5 个 Qp task
- When PopBatch(max_count=3)
- Then 返回 3：1 个 Qv + 2 个 Qp

---

## Feature: ResultCollector

### Scenario: 基本 Top-K 收集
- Given top_k=3 的 ResultCollector
- When TryInsert(id=1, dist=5.0)
- And TryInsert(id=2, dist=3.0)
- And TryInsert(id=3, dist=7.0)
- Then Size() == 3, Full() == true
- And TopDistance() == 7.0

### Scenario: 替换堆顶
- Given top_k=3，已插入 dist=[5.0, 3.0, 7.0]
- When TryInsert(id=4, dist=4.0)
- Then 返回 true（4.0 < 7.0，替换成功）
- And TopDistance() == 5.0
- And Size() == 3

### Scenario: 拒绝过大距离
- Given top_k=3，已插入 dist=[5.0, 3.0, 4.0]
- When TryInsert(id=5, dist=6.0)
- Then 返回 false（6.0 > 5.0）
- And Size() == 3

### Scenario: Finalize 排序
- Given top_k=3，已插入 (id=1, 5.0), (id=2, 3.0), (id=3, 4.0)
- When Finalize()
- Then 返回 [(id=2, 3.0), (id=3, 4.0), (id=1, 5.0)]（distance 升序）

### Scenario: 堆未满时全部接受
- Given top_k=5 的空 ResultCollector
- When TryInsert 3 次
- Then 全部返回 true
- And Full() == false

---

## Feature: RerankConsumer

### Scenario: VEC_ONLY 消费 — 进入 TopK
- Given query_vec 和 dim=128 的 RerankConsumer
- And ResultCollector(top_k=10) 未满
- When Consume 一个 VEC_ONLY CompletedRead（buffer 含 128×float 向量）
- Then collector.Size() 增加 1
- And stats.total_reranked 增加 1

### Scenario: VEC_ONLY 消费 — 距离过大不进入 TopK
- Given collector 已满（10 条），TopDistance=2.0
- When Consume 一个 VEC_ONLY CompletedRead，其向量与 query 距离=5.0
- Then collector.Size() 仍为 10
- And TopDistance() 仍为 2.0

---

## Feature: OverlapScheduler

### Requirement: OverlapScheduler
在保持现有结果正确性和 recall 语义的前提下，`OverlapScheduler` SHALL 支持 resident 专用查询路径。当 `clu_read_mode=full_preload` 且 `use_resident_clusters=1` 时，系统 MUST 允许使用 resident query hot path、可复用 query wrapper 以及 resident 条件下的轻量候选提交组织方式。`coarse_select_ms` MUST 继续独立于 `probe_time_ms` 统计；`probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms` 和 `probe_submit_ms` MUST 继续作为 resident query 主路径的正式输出字段。对于 padded-Hadamard 索引，查询管线 MUST 在 query 开头按 metadata 中的 `logical_dim` / `effective_dim` 规则执行 zero-padding，并使用 query-once Hadamard rotation 后的 pre-rotated prepare 路径，而不是退回随机旋转矩阵路径。

### Scenario: 端到端小规模搜索
- Given 一个已构建的 IVF 索引（N=1000, dim=128, nlist=4）
- And PreadFallbackReader
- When Search(query_vec, top_k=10, nprobe=2)
- Then 返回 10 个 SearchResult
- And 结果按 distance 升序排列
- And stats.total_probed > 0
- And stats.total_io_submitted > 0

### Scenario: 全部 SafeOut 时不发 I/O
- Given 一个索引，且 ConANN epsilon 极大（所有 candidate 都 SafeOut）
- When Search(query_vec, top_k=10, nprobe=1)
- Then 返回 0 个结果
- And stats.total_io_submitted == 0

### Scenario: nprobe=1 单 cluster
- Given 一个索引（nlist=4, cluster_0 有 100 条记录）
- When Search(query_vec, top_k=5, nprobe=1)
- Then 返回 min(5, non-SafeOut count) 个结果
- And 所有结果的 vec_id 解码后 cluster_id == 最近 cluster

### Scenario: Probe + I/O overlap
- Given 一个索引（nlist=8, 每 cluster 200 条）
- And probe_batch_size=64, io_batch_size=32
- When Search(query_vec, top_k=10, nprobe=4)
- Then 完成搜索（不死锁、不遗漏）
- And stats.total_probed == sum of non-SafeOut cluster sizes

### Scenario: Resident hot path exposes query-path timings
- **WHEN** 查询使用 `full_preload + use_resident_clusters=1`
- **THEN** 搜索统计 MUST 独立输出 `coarse_select_ms`
- **AND** MUST 独立输出 `probe_prepare_ms`、`probe_stage1_ms`、`probe_stage2_ms` 和 `probe_submit_ms`
- **AND** `probe_time_ms` MUST 保持为这些 probe 子阶段的兼容聚合字段，而不包含 `coarse_select_ms`

### Scenario: Resident single-assignment serving may use lightweight submit organization
- **WHEN** resident 查询路径满足 single-assignment 条件
- **THEN** 系统 MAY 使用 resident 专用轻量候选提交组织方式
- **AND** 最终 recall 语义与结果排序 MUST 与参考路径保持一致

### Scenario: Padded-Hadamard query uses query-once padded rotation
- **WHEN** 查询运行在 `logical_dim` 非 2 的幂且 metadata 标记为 padded-Hadamard 的索引上
- **THEN** query wrapper MUST 先把 query 从 `logical_dim` zero-pad 到 `effective_dim`
- **AND** MUST 在 query 开头只执行一次 Hadamard rotation
- **AND** probe 期间 MUST 复用 pre-rotated query path

## ADDED Requirements (from quantize-lut-fusion-simd-optimization)

### Requirement: Query pipeline SHALL treat survivor compaction and batch classify as formal SIMD boundaries
resident 查询路径在完成 block-driven submit 之后，系统 MUST 将 Stage1 survivor compaction 和 batch classify 视为正式的下一轮 SIMD 边界。后续优化 MAY 保持结果语义不变，但 MUST 允许从 SafeOut mask 到 survivor index、再到 SafeIn/Uncertain 分类的批量处理，而不继续把 `mask` 之后的主路径固定为逐 lane 控制流。

#### Scenario: Survivor compaction is a formal optimization boundary
- **WHEN** 一个 FastScan block 产生多个非-SafeOut 候选
- **THEN** 系统 MUST 允许批量提取 survivor index
- **AND** 该边界 MUST 可作为独立优化点继续演进

#### Scenario: Batch classify preserves result semantics
- **WHEN** Stage1 采用 batch classify 处理 survivor
- **THEN** recall、最终排序和 SafeIn/Uncertain 语义 MUST 与参考路径保持一致

### Requirement: Query pipeline SHALL expose address batch decode and submit-prep as follow-up SIMD candidates
在 resident 查询路径下，`DecodeAddressBatch` 与 batch submit 前整理 SHALL 被视为正式的后续 SIMD / layout 优化点。实现 MAY 暂时保留标量内部逻辑，但系统 MUST 不把这两段重新隐藏回逐候选提交流程中。

#### Scenario: Address batch decode remains batch-first
- **WHEN** resident 路径处理一个 candidate batch
- **THEN** 地址解码 MUST 保持批量接口和批量语义

#### Scenario: Submit preparation remains batch-first
- **WHEN** resident 路径准备一次 batch submit
- **THEN** slot 分配、class 分组或后续整理 MUST 可以按 batch 粒度继续优化

---

## Feature: ClusterStoreReader Codes 缓存

### Scenario: 全量缓存后 GetCodePtr 返回有效指针
- Given 一个已 Open 的 ClusterStoreReader
- When EnsureClusterLoaded(cluster_id=0)
- Then GetCodePtr(0, 0) 返回非空指针
- And GetCodePtr(0, num_records-1) 返回非空指针
- And 返回的 code 数据与逐条 LoadCode 一致

### Scenario: codes 缓存与 LoadCode 一致性
- Given 已 EnsureClusterLoaded 的 cluster
- When 对所有记录分别调用 GetCodePtr 和 LoadCode
- Then 两种方式返回的 code 数据完全一致

## ADDED Requirements (from resident-thinpath-batch-rerank-optimization)

### Requirement: Resident 查询管线必须支持专用 thin path
查询管线 MUST 支持在 resident full-preload 模式下切换到专用 thin path，并在该路径中仅保留 query 相关的准备、阈值判断、probe、候选输出、rerank 和最终 payload 获取。

#### Scenario: Resident 查询走专用管线
- **WHEN** 查询运行于 resident full-preload 模式
- **THEN** 查询管线必须走 resident 专用 thin path

### Requirement: 查询管线必须支持候选收集后批量 rerank
查询管线 MUST 支持 probe 阶段先输出候选集合，再由后段统一执行原始向量读取、批量距离计算和 top-k 合并，而不是要求每个候选到达后立即完成 rerank。

#### Scenario: 候选先收集后统一处理
- **WHEN** probe 阶段结束一个 cluster 或完成全部目标 cluster 的 probe
- **THEN** 查询管线必须能够对已收集的候选执行批量 rerank 处理

### Requirement: 查询管线优化不得改变 recall 计算口径
resident thin path 和 batched rerank 引入后，查询管线 MUST 保持与优化前一致的 recall 计算口径和结果定义。

#### Scenario: Recall 口径保持不变
- **WHEN** 使用同一数据集、索引和 benchmark 参数比较优化前后结果
- **THEN** 系统必须按相同 recall 定义进行统计和输出

### Requirement: 查询管线必须支持沿用现有提交路径的全 probe 后统一 batch rerank
查询管线 MUST 支持 probe 阶段继续沿用现有 SafeIn 和 Uncertain 各自的提交与解码方式，并在全部目标 cluster 处理完成后，由后段统一执行候选整理、内存池读取、批量距离计算和 top-k 合并，而不是要求每个候选到达后立即完成 rerank。

#### Scenario: 全 probe 后统一处理
- **WHEN** probe 阶段完成全部目标 cluster 的处理
- **THEN** 查询管线必须能够对已收集候选执行统一的 batch rerank 处理

#### Scenario: 查询管线区分两类原始向量读取
- **WHEN** 查询进入 probe 阶段和后续 batch rerank 阶段
- **THEN** 查询管线必须区分"probe 期间按 cluster 分批从磁盘预读到内存池"和"all-probe-done 后从内存池批量读取并执行 rerank"这两类原始向量读取

### Requirement: 查询管线必须输出稳定分段统计
resident thin path 和 batched rerank 引入后，查询管线 MUST 输出稳定、可解释的分段统计，至少包括 `probe_ms`、`prefetch_submit_ms`、`prefetch_wait_ms`、`safein_payload_prefetch_ms`、`candidate_collect_ms`、`pool_vector_read_ms`、`rerank_compute_ms`、`remaining_payload_fetch_ms`、`e2e_ms`、`num_candidates_buffered`、`num_candidates_reranked`、`num_safein_payload_prefetched`、`num_remaining_payload_fetches`。

#### Scenario: 查询结果包含分段统计
- **WHEN** 查询完成并输出 benchmark 结果
- **THEN** 结果中必须包含约定的分段时间和候选计数字段

## ADDED Requirements (from coarse-centroid-score-simd-optimization)

### Requirement: OverlapScheduler / query path SHALL allow coarse selection to use prebuilt packed centroid layouts
The query pipeline SHALL allow coarse cluster selection to consume a centroid layout prepared before query execution, so that the coarse score kernel can focus on computation rather than layout conversion.

#### Scenario: Packed coarse layout is used transparently during query
- **WHEN** `FindNearestClusters()` runs on an index that prepared a coarse packed layout during `Open()`
- **THEN** the query path SHALL use that packed layout for coarse scoring
- **AND** the rest of the query pipeline SHALL continue to receive the same ordered cluster IDs as before

#### Scenario: Query normalize and top-n selection remain semantically unchanged
- **WHEN** the coarse centroid score kernel is optimized
- **THEN** the query pipeline SHALL keep the existing query normalize behavior for cosine serving
- **AND** it SHALL keep the existing top-n selection semantics after score computation

## ADDED Requirements (from probe-submit-windowed-batch-prepread-optimization)

### Requirement: Query pipeline SHALL support cluster-end windowed submit
查询管线 MUST 支持在每个 probed cluster 结束时，对当前累计待发 I/O 请求执行 windowed batch submit。系统 MUST 以待提交请求数作为窗口判断依据；当累计待发请求数不超过阈值时，必须一次性提交全部请求；当累计待发请求数超过阈值时，必须按固定粒度分批提交。首版实现 MUST 支持 `8` 或 `16` 作为固定提交粒度。

#### Scenario: Cluster-end flush submits all requests under threshold
- **WHEN** 一个 cluster probe 完成，且当前累计待提交请求数小于或等于 window threshold
- **THEN** 查询管线必须在该 cluster 结束时一次性提交全部待发请求
- **AND** 不得将这些请求继续延迟到后续 cluster

#### Scenario: Cluster-end flush chunks requests over threshold
- **WHEN** 一个 cluster probe 完成，且当前累计待提交请求数大于 window threshold
- **THEN** 查询管线必须按固定粒度分批提交这些请求
- **AND** 每个提交批次的请求数必须不超过当前 window threshold

### Requirement: Query pipeline SHALL preserve CRC and classification semantics under windowed submit
引入 windowed submit 后，查询管线 MUST 保持现有 `SafeIn` / `Uncertain` 分类语义、`VEC_ONLY` / `ALL` 选择语义、CRC cluster-local merge 语义和 early-stop cluster 边界不变。

#### Scenario: Submit timing changes without changing candidate semantics
- **WHEN** 查询路径启用 cluster-end windowed submit
- **THEN** `SafeIn` 候选仍然必须走 `ALL` 路径
- **AND** `Uncertain` 候选仍然必须走 `VEC_ONLY` 路径
- **AND** CRC estimate merge 与 early-stop 判定仍然必须仅在 cluster 边界更新

## ADDED Requirements (from ip-exrabitq-two-stage-optimization)

### Requirement: Query Pipeline Must Expose Stable Hotspot Boundaries
The query pipeline SHALL expose stable timing and profiling boundaries for coarse selection, prepare, Stage 1, Stage 2, submit, and rerank so that optimization work can be evaluated without conflating multiple segments.

#### Scenario: Fine-grained timing reports stage boundaries
- **WHEN** fine-grained timing is enabled for benchmark analysis
- **THEN** the system SHALL continue to report stable prepare, Stage 1, Stage 2, and submit boundaries that can be compared across optimization rounds

#### Scenario: Clean perf excludes fine-grained timing pollution
- **WHEN** clean perf is run with `fine_grained_timing=0`
- **THEN** the query pipeline SHALL not execute prepare sub-stage timing code that materially pollutes the query CPU hotspot profile

### Requirement: Stage 2 Optimization Shall Preserve Query Semantics
The query pipeline SHALL preserve query semantics when optimizing Stage 2 `IPExRaBitQ`, including candidate ordering behavior, SafeIn/SafeOut semantics, and downstream rerank eligibility.

#### Scenario: Stage 2 kernel replacement preserves classification behavior
- **WHEN** the Stage 2 kernel is replaced by an optimized implementation
- **THEN** the resulting Stage2 distance estimates and classification outcomes SHALL remain compatible with the existing query pipeline semantics

#### Scenario: Later layout upgrades remain pipeline-compatible
- **WHEN** a later stage introduces `sign bit-pack` or compact ex-code layout
- **THEN** the query pipeline SHALL continue to present an equivalent Stage2 query contract to `ClusterProber` and downstream consumers

## ADDED Requirements (from ipexrabitq-stage2-dualpath-optimization)

### Requirement: Query pipeline SHALL support a packed-sign dedicated Stage2 kernel path
The query pipeline SHALL support a Stage2 execution path that dispatches candidates with v10 packed-sign ExRaBitQ payloads to a packed-sign dedicated `IPExRaBitQ` kernel.

#### Scenario: Packed-sign candidate dispatches to dedicated Stage2 path
- **WHEN** the query pipeline boosts a candidate whose Stage2 payload is v10 packed-sign
- **THEN** the pipeline SHALL dispatch that candidate through the packed-sign dedicated Stage2 kernel path

### Requirement: Query pipeline SHALL support a batch Stage2 boosting path
The query pipeline SHALL support a Stage2 batch boosting path that collects uncertain candidates and boosts them as a batch while preserving the same top-k result semantics.

#### Scenario: Stage2 boosting happens after candidate collection
- **WHEN** batch Stage2 boosting mode is active
- **THEN** the query pipeline SHALL be able to collect uncertain candidates before issuing Stage2 boosting work
- **AND** the final top-k semantics SHALL match the non-batch path

### Requirement: Query pipeline SHALL preserve serving semantics across both Stage2 routes
The query pipeline SHALL preserve recall, ranking, CRC, payload, and resident-serving semantics regardless of whether Stage2 uses the packed-sign dedicated kernel route or the batch boosting route.

#### Scenario: Stage2 route does not change serving semantics
- **WHEN** the query pipeline is run with the same query, index, and serving parameters under different Stage2 optimization routes
- **THEN** the serving result semantics SHALL remain unchanged

## ADDED Requirements (from exrabitq-storage-format-upgrade)

### Requirement: Query pipeline SHALL support packed-sign ExRaBitQ Stage2 evaluation
查询路径 MUST 支持在 `bits > 1` 且索引使用 packed-sign ExRaBitQ storage 版本时，直接以 packed sign 作为 Stage2 `IPExRaBitQ` 的输入，而不是继续要求逐维 `sign[dim bytes]` 作为查询期主表示。

#### Scenario: Stage2 consumes packed sign directly
- **WHEN** 查询路径在新格式 `.clu` 上执行 Stage2 ExRaBitQ estimation
- **THEN** `IPExRaBitQ` 必须能够直接消费 packed sign
- **AND** 查询路径不得要求解析阶段先把 packed sign 反解为逐维 byte-sign 才能进入 Stage2

### Requirement: Packed-sign query path SHALL preserve current recall and ranking semantics
引入 packed-sign ExRaBitQ storage 后，query pipeline MUST 保持与旧 byte-sign 格式一致的 recall 口径、结果排序语义和 resident serving 语义，不得因为 sign 存储形式变化而改变 top-k 定义。

#### Scenario: New storage format does not change result semantics
- **WHEN** 使用同一 query 集、相同索引内容和相同搜索参数比较 byte-sign 格式与 packed-sign 格式
- **THEN** 查询输出的 recall 定义必须一致
- **AND** 最终 top-k 排序语义必须一致

### Requirement: Packed-sign query path SHALL remain cross-dimension compatible
查询路径中的 packed-sign Stage2 实现 MUST 适用于不同常见维度，而不能只对单一维度硬编码。查询系统 SHALL 在 resident + IVF 路径下继续支持跨维度 block 化 Stage2 执行。

#### Scenario: Packed-sign Stage2 supports multiple dimensions
- **WHEN** 系统在不同常见 embedding 维度上运行 packed-sign ExRaBitQ Stage2
- **THEN** 查询路径必须继续保持正确性
- **AND** 不得要求只在单一维度上才能进入 packed-sign 主路径

## ADDED Requirements (from ipexrabitq-compact-layout-rebuild)

### Requirement: Query pipeline SHALL expose a compact Stage2 batch-block view
The query pipeline SHALL expose a batch-block Stage2 view for compact `v11` indexes, rather than requiring the Stage2 hot path to reconstruct a batch from independent per-candidate pointers.

#### Scenario: Parsed cluster exposes compact Stage2 block view
- **WHEN** a resident or parsed cluster is opened from a `v11` compact index
- **THEN** the query pipeline SHALL be able to retrieve a Stage2 batch-block view from that cluster
- **AND** that view SHALL describe a compact Stage2 block rather than a single candidate payload

### Requirement: Query pipeline SHALL support compact Stage2 block-aware dispatch
The query pipeline SHALL support dispatching Stage2 boosting by compact batch block and lane selection, rather than only by uncertain candidate list iteration.

#### Scenario: Stage2 dispatch follows block order
- **WHEN** Stage1 produces uncertain candidates for a `v11` compact cluster
- **THEN** the query pipeline SHALL transform them into block-aware Stage2 dispatch units
- **AND** it SHALL drive Stage2 boosting from those block-aware units

### Requirement: Compact Stage2 integration SHALL preserve query semantics
Introducing compact Stage2 block views and block-aware dispatch SHALL NOT change recall computation, final result ordering, CRC semantics, payload semantics, or resident serving semantics.

#### Scenario: Compact Stage2 path preserves serving contract
- **WHEN** the query pipeline serves a query from a compact `v11` index
- **THEN** the final query contract SHALL remain compatible with the existing serving path
- **AND** compact Stage2 integration SHALL not require changes to the user-visible search interface

## ADDED Requirements (from query-hotpath-submit-batch-stage2-refinement)

### Requirement: Query pipeline SHALL treat adaptive submit batching as the primary probe-submit follow-up path
查询主路径在 resident/full-preload 主验收模式下 MUST 将 `probe_submit` follow-up 优化收敛到自适应 submit batching，而不是继续把逐 cluster 立即提交或单一静态阈值视为唯一正式路径。该路径 MUST 保持 `SafeIn` / `Uncertain` 分类、rerank、payload 和最终排序语义不变。

#### Scenario: Adaptive submit changes scheduling but not candidate semantics
- **WHEN** resident 查询路径启用三层 flush 的 `submit-batch`
- **THEN** `SafeIn` 候选仍然 MUST 保持原有的 `ALL` 读取语义
- **AND** `Uncertain` 候选仍然 MUST 保持原有的 `VEC_ONLY` 读取语义
- **AND** rerank、payload 获取和最终 top-k 语义 MUST 与参考路径一致

### Requirement: Query pipeline SHALL preserve probe semantics under fixed-nprobe and CRC early-stop submit control
查询主路径在引入自适应 `submit-batch` 后 MUST 同时保持固定 `nprobe` 路径和 CRC early-stop 路径的 probe 语义。系统 MUST 允许在 fixed-nprobe 模式下通过 final flush 收敛残余请求，并在 early-stop 模式下通过 stop-sensitive flush 保持 stop 判定与请求生命周期一致。

#### Scenario: Fixed-nprobe submit control preserves full probe coverage
- **WHEN** 查询以固定 `nprobe` 模式运行
- **THEN** 自适应 submit 控制 MUST 不减少计划探测的 cluster 数
- **AND** probe 循环结束时 MUST 能通过 final flush 收敛残余请求

#### Scenario: Early-stop submit control preserves CRC stop boundary
- **WHEN** 查询以 CRC early-stop 模式运行
- **THEN** 自适应 submit 控制 MUST 不改变 CRC stop 的 cluster 边界语义
- **AND** stop 判定前后的 flush / drain 行为 MUST 不改变最终 recall 与排序语义

### Requirement: Query pipeline SHALL refine Stage2 through collect/scatter before layout escalation
查询主路径在推进 Stage2 follow-up 优化时 MUST 先把现有 Stage2 路径的 `collect` 与 `scatter` 视为正式优化边界，而不是直接要求新的 resident layout 或 compute-ready 视图。该顺序 MUST 保持现有 resident view、当前 kernel 入口和最终 funnel 语义兼容。

#### Scenario: Stage2 follow-up remains layout-compatible
- **WHEN** resident 查询路径继续优化 Stage2
- **THEN** 系统 MUST 允许仅通过 `collect` 组织和 `scatter` 数值分类的优化来推进
- **AND** 不得把新索引格式或 resident layout 重构作为该阶段的前置条件

#### Scenario: Stage2 refinement preserves funnel semantics
- **WHEN** Stage2 `collect` 或 `scatter` 被优化
- **THEN** Stage1 `SafeOut` / `SafeIn` / `Uncertain` funnel 语义 MUST 保持不变
- **AND** 最终 recall 与结果排序 MUST 与参考路径保持一致

### Requirement: Query pipeline follow-up work SHALL stay attributable to stable sub-phases
本轮 `submit-batch` 与 Stage2 follow-up 优化 MUST 继续挂接到稳定的 query 子阶段拆分上。系统 MUST 保持能够把收益归因到 `probe_submit`、Stage2 collect/scatter 或其他已定义的子阶段，而不是重新把这些成本混回粗粒度总时间。

#### Scenario: Follow-up optimization remains sub-phase attributable
- **WHEN** 后续实现改变 `submit-batch` 或 Stage2 `collect/scatter`
- **THEN** benchmark 和 profiling 输出 MUST 仍然能够定位变化影响的 query 子阶段
- **AND** 不得仅输出无法解释来源的单一总时间变化
