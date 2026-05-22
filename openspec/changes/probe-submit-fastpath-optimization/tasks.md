## 1. Submit 路径可观测性

- [x] 1.1 扩展 `SearchStats`，增加 vector-only/all/payload 请求计数、fixed-buffer 命中/未命中计数、vector-only emit 计时、pending-slot 计时、prep-read 计时，以及 rerank 向量分配/拷贝计时。
- [x] 1.2 更新 `bench_e2e` 的 JSON 和 summary 输出，导出新的 submit-path 与 rerank-storage 诊断字段。
- [x] 1.3 在 `EmitPendingDataRequests`、vector-only buffer acquisition、pending-slot allocation、io_uring prep-read 和 `RerankConsumer::ConsumeVec` 中加入低开销埋点。
- [x] 1.4 运行一次 MSMARCO `fht_kac_rotator` query-only resident/full-preload 基线 benchmark，并记录新增诊断字段。

## 2. Vector-Only Submit Fast Path

- [x] 2.1 将 resident 热路径里的 `pending_vec_only_plans_` 存储替换成可复用的 vector/ring 队列状态，并在每个 query 开始时显式重置。
- [x] 2.2 增加紧凑的 vector-only read-plan 表示，只保存 vector 读取所需的地址/offset 信息。
- [x] 2.3 增加轻量 vector-only pending-slot 分配 helper，只记录 `VEC_ONLY` dispatch 所需的 completion 状态。
- [x] 2.4 将 `EmitPendingDataRequests` 拆成 vector-only 与 all-read 两条 emit 路径，同时为非受控模式保留现有通用行为。
- [x] 2.5 增加聚焦测试或 benchmark 校验，覆盖空队列、部分 flush、尾部 flush，以及不重复/不遗漏的 vector-only 请求发射。

## 3. io_uring Fixed-File Prep

- [x] 3.1 为 `IoUringReader` 增加一个支持 cached fixed-file index 的 registered-buffer 读取 API。
- [x] 3.2 在 scheduler 或 reader 初始化阶段缓存 data file 的 registered index，前提是文件注册成功。
- [x] 3.3 在 vector-only fast path 中使用 cached fixed-file prep API，并在 cached index 不可用时保留 fd-based prep-read fallback。
- [x] 3.4 验证 shared-reader 和 isolated-reader 模式下的 submit、poll、wait、final-drain 和 in-flight 统计行为保持一致。

## 4. Rerank 向量 Slab

- [x] 4.1 在 `RerankConsumer` 中增加可复用的 per-query 向量存储，并支持 query 边界重置。
- [x] 4.2 修改 `ConsumeVec`，在 steady state 下把向量字节复制到可复用 slab，而不是对每个候选执行 `aligned_alloc`。
- [x] 4.3 确保 buffered candidate 的向量指针在 `ExecuteBuffered` 完成之前保持稳定。
- [x] 4.4 保留 `ConsumeAll`、`ConsumePayload`、`TakePayload` 和最终 missing-payload fetch 的 payload cache 行为。
- [x] 4.5 为 rerank slab reset、`ExecuteBuffered` 期间的向量生命周期、候选地址关联和 top-k 等价性增加测试。

## 5. 验证与调优

- [ ] 5.1 运行受影响的 query、async-reader、buffer-pool、rerank-consumer 和 e2e benchmark 测试。
- [ ] 5.2 在 MSMARCO `fht_kac_rotator` query-only resident/full-preload benchmark 上做优化前后对比，检查 `avg_query_time_ms`、`probe_submit_ms`、`probe_submit_prepare_vec_only_ms`、`uring_submit_ms`、`io_wait_ms` 和 candidate 数量。
- [ ] 5.3 对优化后的路径运行 `perf stat` 和延迟启动的 `perf record`，确认 allocator 和 vector-only submit 的开销下降。
- [ ] 5.4 在结构性改动之后扫描 `submit_batch` 和 `io_queue_depth`，确定新的最优运行点。
- [ ] 5.5 通过 ground truth 或确定性的 baseline 对比确认 recall/top-k 语义没有变化。
