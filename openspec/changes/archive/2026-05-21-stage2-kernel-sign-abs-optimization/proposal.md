## 为什么要做

当前 `full_preload + use_resident_clusters=1 + crc=1` 的主工作点上，Stage2 kernel 已经成为下一轮可观测的 query 热点。最新 profiling 显示，`stage2_kernel_ms` 的主要时间都落在 `sign_flip` 和 `abs_fma` 两段，而 `collect`、`scatter`、`tail`、`reduce` 都明显更薄。这说明现在最值得做的，不是继续扩大 Stage2 框架，也不是直接改存储布局，而是先在不改 layout 的前提下，把 compact kernel 的内部重复计算压缩掉，并验证 lane-batched 方向是否真的有收益。

第二阶段原型已经实现为 preload/load-time transcode + parallel-friendly resident view。首轮测量显示：
- 低开销口径下 `probe_stage2` 只有小幅下降，`avg_query` 基本持平
- `parallel_view_build_ms` 和 `resident_parallel_view_bytes` 明显上升
- 细粒度口径下新 kernel 仍未形成真正的 lane-batched 并行收益
这意味着第二阶段方向是值得验证的，但当前实现还只是“更好的输入表示 + 仍然偏 lane 串行的消费”，后续还需要继续优化新 kernel 的并行消费方式。

第三阶段的目标就是在现有 parallel-friendly resident view 上，把 query-time kernel 真正改成小批量 lane-batched 消费：
- 先从 `2-lane` 开始，而不是继续扩大 scratch 或直接上 `8 lanes`
- 保持 `sign prepare boundary` / `abs consume boundary` 不变
- 把中间态限制在寄存器驻留或极小局部状态里，避免回到第一阶段那种显式大 scratch 物化
- 通过更深的 lane batching 进一步压低 `stage2_kernel_sign_flip` 和 `stage2_kernel_abs_fma`

第三阶段已经实现并验证了 `2-lane` / `4-lane` lane-sharing kernel，但结果没有达到性能门槛：
- 低开销口径下 `probe_stage2` 基本持平或略差
- 细粒度口径下 `stage2_kernel_sign_flip` / `stage2_kernel_abs_fma` 也没有形成稳定下降
- 因此第三阶段当前应被视为“实现完成但未达性能门槛”，不再继续向更宽的 lane batching 放大

当前更具体的实现顺序应当是：
- 第一优先级先做方案 A，只重排 `sign prepare` 的共享粒度，验证 lane 之间是否能更便宜地共享 sign 处理
- 第二优先级再做方案 B，把 `sign + abs` 一起纳入 lane 共享调度
- 不直接把 2-lane 扩成 4-lane，除非前两步已经证明共享粒度的重排确实有效

## 这次改什么

- 优化 `IPExRaBitQBatchPackedSignCompact()` 的内部计算路径，但第一步不改 layout，而是在 kernel 内引入 lane-batched scratch，把当前“按 lane 串行、按 dim SIMD”的 workflow 改造成“按 slice 分阶段处理”的 workflow。
- 第一阶段的重点不是继续做局部微调，而是先做一个 layout-compatible 的结构重排：
  - 对一个 `64/32/16` slice，先批量生成多个 lane 的 signed query scratch
  - 再使用 scratch 批量消费 abs magnitude 并累积 dot/bias
  - 先验证 kernel 内 lane batching 是否真的能压低 `sign_flip` 和 `abs_fma`
- 第二阶段才根据第一阶段的结果，决定是否继续推进到 preload-time transcode：
  - 在 cluster load / preload 阶段，把现有 compact layout 转成更适合 lane 并行的 parallel-friendly Stage2 view
  - 让 query-time kernel 直接消费转码后的 view，而不是在热路径里继续做 layout decode
- 对 abs/FMA 侧继续保留独立优化目标：收紧 `LoadAbsMagnitude16Avx512()` 的消费链、减少每 lane 重复的 16B abs 解包/转换、并在不改变语义的前提下重新审视 bias 累积是否需要与 FMA 主链分离。
- 保持 v11 compact-block 存储布局不变，不引入索引重建或格式迁移。
- 保留 Stage2 profiling 口径，并继续输出 kernel 子段计时，便于验证 sign/abs 优化到底命中了哪里。
- 继续保持 Stage2 candidate 分类语义、CRC 行为和 recall 结果不变。

## 影响范围

### 变更的能力
- `exrabitq-stage2-kernel`：收紧 Stage2 compact kernel 的实现要求，先以 kernel 内 lane-batched scratch 为第一阶段主方案，再在必要时扩展到 preload-time transcode 的并行友好视图。

### 影响的代码
- `src/simd/ip_exrabitq.cpp`
- `include/vdb/simd/ip_exrabitq.h`
- `src/index/cluster_prober.cpp` 中的 Stage2 调用点
- `src/index/cluster_prober.cpp`、`src/query/overlap_scheduler.cpp`、`benchmarks/bench_e2e.cpp` 中的 Stage2 profiling 汇总

### 影响的系统
- CRC 下的 resident query 路径
- ExRaBitQ compact Stage2 kernel
- Stage2 子阶段的 benchmark / profiling 归因

这次 change 不引入存储格式变化，不改变 recall 语义，也不重写 Stage2 framework。
