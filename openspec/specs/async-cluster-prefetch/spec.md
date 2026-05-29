# Spec: Async Cluster Prefetch

## Overview

将聚类数据加载从同步 pread 改为 io_uring 异步预取，通过滑动窗口机制与现有查询流水线的原始向量读取共享同一 io_uring ring，实现 cluster I/O、probe CPU 计算、rerank 三者的全程流水线化。

## Requirements

### R1: 独立 ParsedCluster 结构
- 新建 `ParsedCluster` struct，独立于 `ClusterStoreReader::ClusterData`
- `block_buf (unique_ptr<uint8_t[]>)` 持有整个 cluster block buffer 的所有权
- `codes_start (const uint8_t*)` 零拷贝指向 `block_buf` 内部 codes 区域
- `code_entry_size (uint32_t)` 每条 code 的字节数
- `num_records (uint32_t)` 该 cluster 的记录数
- `decoded_addresses (vector<AddressEntry>)` SIMD 解码后的地址表
- Move-only，禁止拷贝

### R2: ClusterStoreReader 最小扩展
- `clu_fd()` 返回 .clu 文件描述符
- `GetBlockLocation(cluster_id)` 从 lookup_table 查询 cluster block 的文件偏移和大小，纯内存查询，返回 `optional<ClusterBlockLocation>`
- `ParseClusterBlock(cluster_id, block_buf, block_size, &out)` 从 raw buffer 解析出 `ParsedCluster`，纯 CPU 操作，不做任何 I/O，block_buf 所有权转移给 out
- **不修改**任何现有接口：ClusterData、EnsureClusterLoaded、GetCodePtr、GetAddress、loaded_clusters_ 全部保持原样
- Segment 转发以上 3 个新接口

### R3: 滑动窗口预取
- 查询开始时批量提交 `prefetch_depth`（默认 16）个 cluster block 读取
- 每 probe 完一个 cluster 后检查 `inflight_clusters_`，低于 `refill_threshold`（默认 2）时补充 `refill_count`（默认 2）个
- `prefetch_depth`、`refill_threshold`、`refill_count` 可通过 SearchConfig 配置

### R4: 严格距离顺序 probe
- 始终按 FindNearestClusters 返回的距离排序顺序 probe cluster
- io_uring CQE 完成顺序可能乱序，通过 `ready_clusters_` map 缓冲
- 在等待特定 cluster 就绪的 WaitAndPoll 循环中，同时消化其他类型的 CQE（VEC_ONLY/VEC_ALL/PAYLOAD 的 rerank）

### R5: ProbeCluster 直接操作 ParsedCluster
- ProbeCluster 接受 `const ParsedCluster&` 参数
- 从 `pc.codes_start + idx * pc.code_entry_size` 取 code（替代 GetCodePtr）
- 从 `pc.decoded_addresses[idx]` 取 address（替代 GetAddress）
- 不调用 EnsureClusterLoaded、GetCodePtr、GetAddress
- `loaded_clusters_` 完全不参与查询路径
- probe 完后 `ready_clusters_.erase(cid)` 释放 ParsedCluster 内存

### R6: 统一事件循环
- cluster block 读取（.clu fd）和原始向量读取（.dat fd）共享同一个 AsyncReader / io_uring ring
- CQE 通过 `PendingIO.type` 区分处理路径：CLUSTER_BLOCK / VEC_ONLY / VEC_ALL / PAYLOAD
- 不使用 InferType 推断（两个 fd 来源无法区分），提交时显式标记类型

### R7: CLUSTER_BLOCK buffer 生命周期
- 由 BufferPool.Acquire 分配
- CQE 完成后所有权转移到 ParsedCluster.block_buf，不通过 BufferPool.Release 回收
- probe 完后 ready_clusters_.erase → ParsedCluster 析构 → block_buf 释放
- 查询结束时 ready_clusters_ 应为空

## Additional Requirements (from clu-full-preload-optimization)

### Requirement: Cluster-side query data SHALL support selectable loading modes
The cluster-side query path SHALL support both the existing sliding-window preload mode and a full `.clu` preload mode, with the loading mode selected explicitly by configuration.

#### Scenario: Sliding-window mode remains available
- **WHEN** the cluster loading mode is set to `window`
- **THEN** the query pipeline SHALL continue to use the existing sliding-window cluster-read path
- **AND** it SHALL preserve the current `prefetch_depth` and refill-based behavior

#### Scenario: Full preload mode bypasses cluster-block submissions
- **WHEN** the cluster loading mode is set to `full_preload`
- **THEN** the query pipeline SHALL obtain cluster-side probe data from resident memory
- **AND** it SHALL NOT submit per-cluster `CLUSTER_BLOCK` I/O requests during the measured query phase

#### Scenario: Mode selection is explicit and comparable
- **WHEN** the benchmark or query pipeline is configured for cluster loading
- **THEN** the selected mode SHALL be externally visible in configuration or output metadata
- **AND** the same query parameters SHALL be runnable under both modes for before/after comparison

### Requirement: Query-time cluster probe SHALL remain compatible with the parsed-cluster access pattern
Regardless of loading mode, the cluster probe path SHALL consume a cluster representation that exposes the same probe-relevant fields required by the current parsed-cluster-based logic.

#### Scenario: Probe path uses a compatible resident representation
- **WHEN** full preload mode is enabled
- **THEN** the cluster probe implementation SHALL receive a resident cluster representation compatible with the existing quantized-code and decoded-address access pattern
- **AND** the change SHALL NOT require a different search algorithm or a different top-k interpretation

## REMOVED Requirements

### Requirement: Sliding-window prefetch is the only cluster-side loading strategy
**Reason**: The system now needs a second mode that fully preloads `.clu` data before queries so the project can compare query-time speed against the previous sliding-window strategy.
**Migration**: Existing behavior remains available under `window` mode. New benchmarks and implementations SHALL treat sliding-window loading as one selectable mode rather than the only supported strategy.

## Constraints

- 单线程执行，无锁
- 与现有 PreadFallbackReader 兼容（fallback 路径同样受益于整块读取 + ParsedCluster）
- 不改变 AsyncReader 接口（PrepRead 已支持任意 fd）
- 不改变 RerankConsumer、ResultCollector、SearchResults 接口
- 不修改 ClusterStoreReader 的现有接口和内部结构

## ADDED Requirements (from probe-submit-windowed-batch-prepread-optimization)

### Requirement: Async prefetch pipeline SHALL support type-bucketed batch PrepRead
异步预取链路 MUST 支持在 query 在线路径中按请求类型对候选进行分桶，并以 batch 方式准备读取计划。系统 MUST 至少区分 `VEC_ONLY` 和 `ALL` 两类请求，并先完成同类请求的批量地址/长度/slot/buffer 准备，再统一发射 SQE。

#### Scenario: Vec-only requests are prepared in a dedicated batch
- **WHEN** 一个 cluster 产生多条 `VEC_ONLY` 候选
- **THEN** 系统必须将这些请求作为一个 `vec_only` 批次完成读取计划准备
- **AND** 不得要求每个候选单独走完整的 `PrepReadVecOnly` 准备路径

#### Scenario: All requests are prepared in a dedicated batch
- **WHEN** 一个 cluster 产生多条 `ALL` 候选
- **THEN** 系统必须将这些请求作为一个 `all` 批次完成读取计划准备
- **AND** 该批次必须继续沿用现有 `ALL` 路径所要求的 payload 可用语义

### Requirement: Async prefetch pipeline SHALL separate batch prepare from submit
异步预取链路 MUST 将"批量准备读取计划"和"触发 submit"作为两个独立阶段处理。`probe_submit_ms` MUST 覆盖用户态批量准备与 SQE 发射成本，而 `uring_submit_ms` MUST 继续仅表示提交动作本身的开销。

#### Scenario: Batch prepare completes before submit decision
- **WHEN** 一个 cluster 内的候选已经完成类型分桶
- **THEN** 系统必须先完成该批次的读取计划准备与 SQE 发射
- **AND** 再根据 submit window 规则决定是否立即提交

#### Scenario: Benchmark can attribute prepare and submit separately
- **WHEN** benchmark 导出查询分段统计
- **THEN** 输出必须能够区分批量 prepare 成本与 `io_uring_submit` 成本
- **AND** 不得把两者合并为单一不可解释字段

## ADDED Requirements (from probe-submit-fastpath-optimization)

### Requirement: Async read preparation SHALL support cached fixed-file data reads
async reader path SHALL 支持在 data file 已注册时，使用缓存的 fixed-file index 来准备 registered-buffer data read。现有基于 fd 的 preparation APIs MUST 继续保留为 fallback。

#### Scenario: Cached fixed-file index is used for hot data reads
- **WHEN** data file descriptor 已注册，且 scheduler 发射一个使用 registered buffer 的 vector-only read
- **THEN** 系统 SHALL 能够使用缓存的 fixed-file index 来准备 SQE
- **AND** 该热点路径 MUST 避免每个请求都做 registered-fd 查找

#### Scenario: Fallback remains available when fixed index is unavailable
- **WHEN** data file 未注册或缓存的 fixed-file index 无效
- **THEN** 系统 SHALL 回退到现有基于 fd 的 prep-read 行为
- **AND** 读取正确性必须保持不变

### Requirement: Async vector-only fast path SHALL preserve io_uring completion semantics
vector-only fast path SHALL 保留现有的 io_uring completion 语义，包括 user-data 到 pending slot 的映射、in-flight 计数、fixed-buffer 释放，以及对 isolated 或 shared reader 模式的兼容性。

#### Scenario: Completion dispatch resolves fast-path vector slots
- **WHEN** 一个 vector-only fast-path read 完成
- **THEN** completion dispatch SHALL 将 user-data token 解析到正确的 pending slot
- **AND** read buffer cleanup path MUST 恰好释放一次 fixed buffer 或 pooled buffer

#### Scenario: Reader mode compatibility is preserved
- **WHEN** scheduler 使用 shared reader 或 isolated reader
- **THEN** vector-only fast-path reads SHALL 保留现有的 submit、poll、wait、final-drain 行为
- **AND** tail drain 或 final drain 中不得丢失任何 completion
