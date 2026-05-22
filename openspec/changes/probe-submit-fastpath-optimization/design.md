## Context

当前 MSMARCO768 `fht_kac_rotator` 的 resident/full-preload 查询路径已经明显是 CPU bound。最近的测量显示：`io_wait ~= 0.002 ms/query`、`probe_submit ~= 1.0-1.1 ms/query`、`submit_prepare_vec_only ~= 1.05 ms/query`，每个 query 大约有 780 个 vector-only read 请求。`perf record` 显示可见开销主要落在 `EmitPendingDataRequests`、`DispatchCompletion`、`RerankConsumer::ConsumeVec` 触发的分配器路径，以及 `Search` 中的内联工作。

相关热点路径如下：

```text
CandidateBatch
  -> BuildReadPlans
  -> pending_vec_only_plans_
  -> EmitPendingDataRequests
  -> AllocatePendingSlot
  -> PrepReadRegisteredBufferTagged
  -> DispatchCompletion
  -> RerankConsumer::ConsumeVec
  -> ExecuteBuffered
```

目标是在不改变现有查询结果语义、io_uring 行为、fallback 路径和 benchmark 可比性的前提下，降低逐候选的 CPU 包装成本。

## Goals / Non-Goals

**Goals:**

- 增加足够的 submit-path 可观测性，以便在优化前后定位 vector-only 请求开销。
- 在安全前提下，让 resident single-assignment 的 `VEC_ONLY` 读取走更紧凑的 fast path，避开通用 read-plan 和 pending-slot 开销。
- 通过可复用的 per-query 向量存储，避免逐候选 rerank 向量分配。
- 保持 recall、top-k 语义、payload 所有权，以及现有 non-resident/window read 行为不变。
- 用 MSMARCO `fht_kac_rotator` 的 query-only perf 和结构化 benchmark 输出验证改动效果。

**Non-Goals:**

- 不修改 Stage1 或 Stage2 的分类 kernel。
- 不修改外部 benchmark CLI 合约。
- 不替换 io_uring，也不移除 pread fallback。
- 不引入多线程查询执行。
- 第一版不做 streaming rerank，继续保留批量 rerank 语义。

## Decisions

### 决策：先优化占比最高的 `VEC_ONLY` 路径

当前测量里 `SafeIn` 几乎为 0，而每个 query 约有 780 个 vector-only 请求。第一版实现应优先引入专门的 `VEC_ONLY` read-plan 表示和 emit 路径，而不是统一优化所有请求类型。

备选方案：一次性重构所有 `PendingIO` 请求类型。这会让 cluster block、payload 和 all-read 的所有权路径一起暴露在改动中，正确性风险更高，而且并不能匹配当前测得的瓶颈。

### 决策：使用紧凑的 plan ring/head-index，而不是 `std::deque`

在 resident 热路径中，`pending_vec_only_plans_` 应改成 vector-backed queue，并用 head index 或 ring 语义管理。这样可以保持分配可预测，同时避免热路径中的 `front/pop_front` 开销。

备选方案：保留 `std::deque`，只调 submit batch size。这样每个请求的固定管理成本仍然存在，结构变化后最优批量点还会继续漂移。

### 决策：引入轻量的 vector-only pending slot

热路径应该支持一种只记录 `VEC_ONLY` completion 处理所需字段的分配 helper：地址、buffer 指针、cleanup 模式，以及必要时的 fixed buffer index。通用 `PendingIO` 结构仍保留给 cluster block、payload 和 fallback 路径。

备选方案：全局改造 `PendingIO`。这样会在不常见路径上引入所有权回归风险，也不利于回滚。

### 决策：为 fixed-file 读取缓存 registered data fd index

`PrepReadRegisteredBufferTagged` 当前会为每个请求检查 registered fd 映射。scheduler 或 reader 应缓存 data fd 的 registered index，并在 vector-only fast path 上使用固定索引的 prep API。

备选方案：保留每次请求都查 fd 的方式。单次查找确实不重，但它会在每个 query 中重复几百次，而且很容易避免。

### 决策：用 per-query slab 替代逐候选 rerank 向量分配

`RerankConsumer::ConsumeVec` 当前会为每个候选分配对齐后的向量存储并复制 read buffer。使用 per-query slab 可以在保留现有批量 rerank 行为的同时，移除 completion 处理中的分配器抖动。

备选方案：在 completion 时直接计算 rerank 距离并立即释放 read buffer。这个方案可能更快，但它会更深入地改变顺序和 collector 时序。

### 决策：先加可观测性，再依赖结构性优化

新的计数器和计时应覆盖 fixed-buffer 命中/未命中、vector-only/all/payload 请求计数、emit/slot/prep 计时，以及 rerank 分配/拷贝计时。这些字段会成为后续优化的验收依据。

备选方案：先实现 fast path，再通过总查询时延推断效果。这样很难把 submit-path 的收益和 Stage1/Stage2 或 coarse select 的噪声区分开。

## Risks / Trade-offs

- buffer 生命周期回归 -> 保留通用路径，同时为 fixed buffer 释放、pool 释放、payload 所有权和 rerank slab 生命周期补上聚焦测试。
- 查询结果回归 -> 用确定性 query 集对比 recall/top-k 输出，和现有路径做一致性比较。
- 更快的 submit path 可能掩盖更慢的 completion copy -> 把 `consume_vec_alloc_ms` 和 `consume_vec_copy_ms` 独立于 `probe_submit_ms` 报告。
- ring queue 状态 bug -> 在每个 query 开始时显式重置 head/tail/capacity，并覆盖空队列、部分 flush 和 tail flush 用例。
- slab 过度分配 -> 初始 slab 按观测到的 candidate 数量设定，需要时再增长；内存释放遵循现有对象生命周期边界。
- 初始化阶段的 perf 噪声 -> 使用 query-only resident/full-preload 运行和延迟 `perf record` 或现有结构化 query 指标进行验证。

## Migration Plan

1. 先增加可观测性字段和 benchmark 输出，不改变行为。
2. 在现有 resident/single-assignment 条件下实现 vector-only read-plan fast path。
3. 增加 cached fixed-file prep 支持，同时保留通用 `PrepReadTagged` fallback。
4. 在保留现有批量 rerank 行为的前提下实现 rerank vector slab。
5. 对 query、async reader、rerank consumer 和 buffer ownership 跑单元测试。
6. 在 MSMARCO `fht_kac_rotator` query-only benchmark 上做前后对比，检查 `probe_submit`、`submit_prepare_vec_only`、`uring_submit`、recall 和 top-k 输出。
7. 在结构变化之后再扫 `submit_batch` 和 `io_queue_depth`，找出新的最佳运行点。
