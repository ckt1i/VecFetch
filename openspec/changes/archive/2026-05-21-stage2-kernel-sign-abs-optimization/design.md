## 背景

当前 resident benchmark 的画像已经很清楚：FastScan prepare 之后，下一个可见热点就是 Stage 2 kernel。  
在 `IPExRaBitQBatchPackedSignCompact()` 里，主要成本落在 `sign_flip` 和 `abs_fma` 两段；`collect`、`tail`、`reduce` 和 traversal 的占比都明显更小。  
这意味着这次 change 的第一阶段应该只盯 kernel 内部的计算结构，不去动 compact-block 存储布局，也不去重写 Stage 2 framework。只有当第一阶段验证 lane-batched 方向成立之后，才进入 preload-time transcode 这一更大的第二阶段方案。

## 目标 / 非目标

**目标：**
- 通过优化 compact kernel 的 sign handling 和 magnitude/FMA 路径，降低 `stage2_kernel_ms`
- 先在不改 layout 的前提下，用 kernel 内 lane-batched scratch 验证更深的 lane 并行是否值得
- 如果第一阶段成立，再把同样的并行友好输入表示前移到 preload/load 阶段
- 保持当前 compact-block layout 和 Stage 2 分类语义不变
- 保留并扩展 profiling 归因，让 sign handling 和 abs/FMA 可以独立验证

**非目标：**
- 不做存储格式迁移，也不引入新的 compact layout
- 不改 CRC 策略，也不改 early-stop 语义
- 不重写 Stage 2 的 collection/scatter 编排
- 第一阶段不做 preload-time transcode，也不直接引入双格式 resident storage

## 决策

### 决策 1：先在 compact kernel 内实现 lane-batched scratch，再碰 Stage 2 周边框架
kernel 本身是主要热点，而 `collect` 和 `scatter` 都更薄。  
第一轮应该直接优化 AVX-512 compact kernel body，但不是继续做局部微调，而是在 `IPExRaBitQBatchPackedSignCompact()` 内引入 lane-batched scratch，把当前 “lane 串行 + dim SIMD” 的混合 workflow 重排成 “按 slice 分阶段处理” 的 workflow。

备选方案：
- 先重做 Stage 2 block packing。拒绝，因为当前数据表明 block traversal 不是瓶颈。
- 直接上 preload-time transcode。暂缓，因为在不知道 lane-batched kernel 是否真的有收益之前，先引入 load-time 转码和双视图管理会把范围放大得过早。
- 只加更多 instrumentation。拒绝，因为热点已经足够明确，可以直接进入代码修改。

第一阶段的具体设计：
- 仍然直接消费现有 `abs_blocks` / `sign_blocks`，不改盘上格式，也不改 resident layout。
- 对每个 `dim_block` 内的 `64/32/16` slice，先统一加载 query slice，再进入两个显式阶段：
  1. `sign prepare phase`
     - 为多个 lane 提取 sign chunk
     - 生成 signed query scratch
     - 在这一阶段内同步累计 bias 所需的 signed query 部分和
  2. `abs consume phase`
     - 使用上一阶段生成的 signed query scratch
     - 批量加载 abs magnitude
     - 更新 dot accumulator
- 第一版建议只做 `2-lane` 或 `4-lane` batching，而不是一上来就覆盖全部 `8 lanes`，避免 scratch、shuffle 和寄存器压力过早失控。

第一阶段的数据结构变化：
- 持久数据结构不变：
  - `ParsedCluster::ExRaBitQBatchBlockView`
  - `abs_blocks`
  - `sign_blocks`
  - `Stage2BlockScratch`
- 仅在 kernel 内新增临时 scratch，例如：
  - `signed_q_cache[slice_subblocks][lane_batch]`
  - 可选的 `sign_chunk_cache[lane_batch]`
- 这些 scratch 只在一次 kernel 调用内有效，不进入 resident cluster 或 query object 的持久状态。

第一阶段前后的 workflow 差异：

优化前：
```text
for block
  for slice
    load q
    for lane
      sign -> flip -> abs -> fmadd -> bias
```

优化后：
```text
for block
  for slice
    load q
    phase A: for lane_batch
      sign -> flip -> signed_q_cache -> bias
    phase B: for lane_batch
      abs -> fmadd using signed_q_cache
```

这一步真正优化的不是某一个 helper，而是把 sign 和 abs/FMA 从一条交错热链里拆开，为后续继续优化提供更稳定的边界。

第一阶段的当前实现结论：
- 已尝试过三种直接实现：
  - `2-lane` 整段 slice scratch
  - `4-lane` 整段 slice scratch
  - `2-lane` 的 16-dim subblock 局部分阶段
- 这三种实现都保持了语义正确，但在低开销 benchmark 下都出现了可见退化，因此当前版本不能直接作为第一阶段的可接受实现。
- 当前观测说明：在现有 compact layout 下，把 `signed_q_cache` 显式物化到 scratch，会引入额外的 store / reload / register-pressure / loop-control 成本，这些成本超过了它减少的 lane 重复工作。
- 因此，第一阶段的下一轮设计不应继续沿着“大块显式 scratch 物化”推进，而应考虑：
  - 更窄的寄存器内 phase split
  - 或者直接进入第二阶段原型验证

### 决策 2：把 kernel 的两个稳定边界定义为 `sign prepare` 和 `abs consume`
profiling 已经表明，sign-flip 和 abs/FMA 是 compact kernel 里最大的两段。  
但在两阶段路线下，决策 2 不应该只停留在“两个热点分别优化”这一层，而应该进一步定义：Stage2 kernel 的稳定内部边界是什么，以及这两个边界如何在第一阶段和第二阶段之间保持一致。

备选方案：
- 先优化 reduction 或 tail。拒绝，因为它们相对太小。
- 先改 storage layout。拒绝，因为这会把问题带到更大的范围，并引入迁移风险。

核心设计：
- kernel 内部应被稳定拆成两个边界：
  1. `sign prepare boundary`
     - 输入：query slice + packed sign slice
     - 输出：`signed_q_cache` 或等价 signed query 中间表示，以及用于最终 `0.5 * signed_q` 的 bias partial
  2. `abs consume boundary`
     - 输入：`signed_q_cache` + abs magnitude slice
     - 输出：dot partial
- 这两个边界是整个 change 的稳定接口：
  - 第一阶段：在不改 layout 的前提下，用 kernel-local scratch 验证这两个边界是否真的有效
  - 第二阶段：如果进入 preload-time transcode，则只改变这两个边界的输入表示，不改变这两个边界的职责

第一阶段里的设计要求：
- sign-focused 优化的边界不再是零散的 memcpy/load/flip 动作，而是完整的 `sign prepare phase`
- abs/FMA-focused 优化的边界不再是单个 helper，而是完整的 `abs consume phase`
- `signed_q_cache` 是第一阶段两个边界之间的显式中间结果，不应再把 sign 和 abs/FMA 混成同一条 lane-local 热链

只有在第一阶段确认 lane-batched scratch 确实压低了 `sign_flip` 或 `abs_fma` 之后，第二阶段才推进到 preload-time transcode。

第二阶段的设计方向：
- 在 cluster load / preload 阶段，对现有 compact layout 做一次转码，生成 parallel-friendly Stage2 view。
- 这个 view 的目标形态是 slice-major、lane-batched，例如：

```text
for each dim_block:
  for each 16-dim slice:
    abs_slice[lane_batch][16]
    sign_slice[lane_batch]
```

- query-time kernel 则直接消费这个 parallel-friendly view，而不是继续在热路径里从 lane-major compact layout 里拆 slice。
- 第二阶段的目的不是重新定义 kernel 算法，而是给同一个 `sign prepare boundary` / `abs consume boundary` 提供更适合 lane-batched 消费的输入表示。
- 第一版建议先保留双视图：
  - original compact view
  - parallel-friendly resident view
  这样方便做 A/B benchmark 和语义对照。

为什么第二阶段有机会消掉第一阶段的退化成本：
- 第一阶段退化的核心，不是“边界设计错误”，而是“边界的当前实现方式过贵”：
  - `signed_q_cache` 需要在 query-time 反复物化
  - 每个 `64/32/16` slice 都要重复做 scratch 写入和回读
  - 在现有 lane-major compact layout 下，scratch 只是把原本的 layout 解码成本换成了另一种 query-time 搬运成本
- 第二阶段如果在 preload/load 阶段预先生成 parallel-friendly resident view，就有机会把下面这些成本前移出 query hot path：
  - sign slice 的抽取和重排
  - abs slice 的 lane-batched 组织
  - 为 `sign prepare boundary` / `abs consume boundary` 准备输入表示
- 如果第二阶段成立，query-time kernel 不再需要反复“生成 scratch 后再消费”，而是直接消费已重排好的 resident view；因此，第一阶段当前版本里最明显的 `scratch/store/reload` 成本理论上是可以被消掉的。
- 但这不等于第二阶段一定更快，因为它只是把成本从 query-time 挪到了 load-time，并额外引入了 resident 内存占用；所以第二阶段必须单独比较：
  - `probe_stage2` 的下降
  - `view_build_ms`
  - `view_bytes`

abs/FMA 进一步说明：
- `LoadAbsMagnitude16Avx512()` 是下一轮 abs 侧优化的正确边界，因为它正好包住了 16B unpack/convert 链。
- bias accumulation 应该和 abs/FMA 一起测，因为它和 FMA 共享同一条 lane-local 依赖链，虽然数学上是分开的。
- 第一阶段的 abs 侧优化应保持现有 compact-block layout 和 lane/block 迭代顺序不变。
- 只有在第二阶段 preload-time transcode 启动后，query-time kernel 才允许切换到新的 parallel-friendly resident view。

### 决策 3：优化必须分阶段推进，并且保持 profiling-friendly
这次 change 必须分两阶段推进：
1. 第一阶段：layout-compatible 的 kernel 内 lane-batched scratch
2. 第二阶段：根据第一阶段结果再决定是否引入 preload-time transcode

两阶段都必须在 benchmark 输出中保留 kernel 子段 attribution。  
profiling 分解不是附属功能，而是决定是否值得进入下一阶段的依据。

### 决策 4：第三阶段在 parallel-friendly resident view 上做 2-lane 寄存器驻留 batching
第二阶段已经验证了 preload-time transcode 能改善输入表示，但 query-time kernel 仍然没有真正吃到 lane-batched 并行收益。  
因此第三阶段应该建立在第二阶段的 parallel-friendly resident view 上，做一个更窄、更稳的 query-time batched kernel：
- 优先从 `2-lane` batching 开始
- `sign prepare` 和 `abs consume` 保持为稳定边界
- 中间态限制在寄存器驻留或极小局部状态里，不再显式物化大块 `signed_q_cache`
- 以是否继续压低 `stage2_kernel_sign_flip` / `stage2_kernel_abs_fma` 作为是否放大到 `4-lane` 的判断依据

第三阶段的已验证实现结论：
- `2-lane` 和 `4-lane` lane-sharing kernel 都已经实现并验证了功能正确性
- 但在当前 COCO resident 工作点上，低开销口径下没有获得稳定收益，`probe_stage2` 基本持平或略差
- 细粒度口径下 `stage2_kernel_sign_flip` / `stage2_kernel_abs_fma` 也没有下降到足以证明 batching 有收益的门槛
- 因此第三阶段应视为“实现完成但未达性能门槛”，不应继续向更宽的 lane batching 放大

第三阶段不是回退到第一阶段旧实现，而是把第一阶段的边界思想与第二阶段的输入表示结合起来，形成真正的 lane-batched consume 路径。

第三阶段的实现顺序应进一步细化为：
- 方案 A：先只重排 `sign prepare` 的共享粒度，让多个 lane 尽可能共享同一轮 sign 处理，abs consume 暂时保持不变
- 方案 B：在方案 A 证明有效后，再把 `sign + abs` 一起纳入 lane 共享调度
- 只有当方案 A 和方案 B 都表明共享粒度的重排真的有效时，才考虑把批宽放大到 `4-lane`
- 现在的 `4-lane` 验证已经表明共享粒度重排并没有带来预期收益，因此第三阶段应在此收口，不再继续向更宽批处理扩展

### 第二阶段原型结果
第二阶段已经以 preload/load-time transcode + parallel-friendly resident view 的形式做了原型验证。当前观测说明：
- 低开销口径下 `probe_stage2` 有小幅下降，但 `avg_query` 基本持平
- `parallel_view_build_ms` 与 `parallel_view_bytes` 带来了明显的 load-time / memory 成本
- 细粒度口径下的新 kernel 还没有形成预期中的 lane-batched 并行收益

第三阶段也已经以 `2-lane` / `4-lane` lane-sharing kernel 的形式做了实现验证。当前观测说明：
- 低开销口径下 `probe_stage2` 没有稳定改善，基本持平或略差
- 细粒度口径下 `stage2_kernel_sign_flip` / `stage2_kernel_abs_fma` 也没有形成稳定下降
- 因此第三阶段不能再被视为“待继续放大”的路线，而应该记录为“实现完成但未达性能门槛”

这说明第二阶段方向本身是成立的，但当前实现还没有把 parallel view 的潜力完全吃出来。下一步优化重点应从“输入表示更好”转向“新 kernel 真正按 lane-batched 方式消费这份输入”。

备选方案：
- 把 kernel 子段折叠回一个总量。拒绝，因为这样会看不出到底是哪一段改善了。

## 风险 / 代价

- [风险] 更细粒度的 kernel timing 会扭曲绝对延迟。→ 规避方式：fine-grained timing 只用于归因，最终性能以低开销 benchmark 为准。
- [风险] 重构 sign handling 可能引入 AVX-512 lane bug 或 packed-sign 语义不一致。→ 规避方式：保留 reference 等价检查，并和现有 compact path 对比 kernel 输出。
- [风险] 第一阶段 lane-batched scratch 可能引入 scratch 搬运、shuffle 或寄存器压力，导致无收益或退化。→ 规避方式：第一版先限制在 `2-lane` 或 `4-lane` batching，并用 profiling 验证 `sign_flip` / `abs_fma` 是否真实下降。
- [风险] 第二阶段 preload-time transcode 可能把 query-time 收益转移成 load-time 和内存占用成本。→ 规避方式：单独记录 `view_build_ms`、`view_bytes`，用 A/B benchmark 判断是否值得保留。

## 迁移计划

1. 先在原地实现第一阶段的 lane-batched scratch kernel，保持存储布局不变。
2. 跑 resident benchmark 的 fine-grained timing，确认 kernel 子段加总仍然等于同一个 `stage2_kernel_ms`，并观察 `sign_flip` / `abs_fma` 是否下降。
3. 对比 low-overhead benchmark 的前后结果，确认总 query 时间和 recall 稳定。
4. 只有在第一阶段证明 lane-batched 方向有收益后，才进入第二阶段 preload-time transcode 设计。
5. 第二阶段里先保留双视图，再比较：
   - 原始 compact view + 第一阶段 kernel
   - parallel-friendly resident view + 新 kernel

## 推荐的下一轮顺序

1. 先实现第一阶段的 `2-lane` 或 `4-lane` scratch-batched kernel，不改 layout。
2. 在第一阶段 kernel 内把 workflow 改成：
   - `sign prepare phase`
   - `abs consume phase`
3. 先观察 `stage2_kernel_sign_flip` 和 `stage2_kernel_abs_fma` 是否都比当前基线更低。
4. 如果第一阶段成立，再实现 preload-time transcode 的 parallel-friendly Stage2 view。
5. 最后再决定是否要把 preload-time transcode 下沉成更长期的 resident 默认表示。

## 开放问题

- 第一阶段 lane-batched scratch 的 batch 宽度应该先选 `2` 还是 `4`。
- 第二阶段是否需要双视图长期共存，还是只作为实验性路径保留。
- profiling 是否继续只保留 `sign_flip / abs_fma / tail / reduce` 的拆分，还是在进入第二阶段后补充 `view_build_ms / view_bytes`。
- 第三阶段应该先做 `2-lane` 还是直接定义可扩展到 `4-lane` 的批处理骨架。
