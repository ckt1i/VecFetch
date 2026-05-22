## 新增要求

### Requirement: Stage 2 compact kernel 应将 sign handling 和 magnitude consumption 作为可分离的热路径组件来优化
系统应将 `IPExRaBitQBatchPackedSignCompact()` 视为一个 compact Stage 2 kernel，其主要 query-time 工作可以通过拆分 sign handling 和 magnitude accumulation 来优化。实现必须保持 kernel 等价，同时允许 sign-flip 和 abs/FMA 被独立测量和独立优化。

#### Scenario: kernel 子步骤仍然可归因
- **WHEN** 优化后的 compact kernel 在开启 fine-grained profiling 时运行
- **THEN** sign handling 和 magnitude accumulation 必须仍然可以作为独立子步骤区分
- **AND** 总 kernel 结果必须与 reference compact kernel 等价

#### Scenario: kernel 优化不需要存储迁移
- **WHEN** compact kernel 针对 sign 和 magnitude handling 做优化
- **THEN** 系统必须继续消费现有的 v11 compact-block ex-code layout
- **AND** 不能要求 index rebuild 或 storage-format migration

### Requirement: Stage 2 compact kernel 应将 `sign prepare` 和 `abs consume` 作为稳定内部边界
系统应将 Stage2 kernel 的内部接口稳定定义为 `sign prepare boundary` 和 `abs consume boundary`。第一阶段和第二阶段都必须围绕这两个边界设计，区别仅在于输入表示不同，而不在于重新定义 kernel 语义。

#### Scenario: 第一阶段通过 `signed_q_cache` 显式分隔两个边界
- **WHEN** 第一阶段 kernel 内 lane-batched scratch 被实现
- **THEN** 系统必须通过 `signed_q_cache` 或等价中间表示，在逻辑上把 `sign prepare` 和 `abs consume` 显式分开
- **AND** 不应继续把 sign 处理和 abs/FMA 消费作为同一条不可分辨的 lane-local 热链执行

#### Scenario: 第二阶段仅改变边界输入，不改变边界职责
- **WHEN** 第二阶段引入 preload-time transcode 或 parallel-friendly resident view
- **THEN** 系统只能改变 `sign prepare boundary` 与 `abs consume boundary` 的输入表示
- **AND** 不得改变这两个边界分别负责的数学工作

### Requirement: Stage 2 compact kernel 应先以 kernel 内 lane-batched scratch 作为第一阶段主方案
系统应先在不改变现有 compact-block layout 的前提下，在 `IPExRaBitQBatchPackedSignCompact()` 内引入 lane-batched scratch。第一阶段实现必须通过 kernel-local scratch 把 sign prepare 和 abs consume 从同一条交错热链里拆开，而不是直接进入 preload-time transcode 或 storage migration。

#### Scenario: 第一阶段仍直接消费现有 compact layout
- **WHEN** 第一阶段 kernel 优化被实现
- **THEN** 它必须继续直接消费现有的 `abs_blocks` 和 `sign_blocks`
- **AND** 不得要求 resident storage 增加新的持久 layout

#### Scenario: 第一阶段应将 workflow 拆成显式阶段
- **WHEN** 第一阶段 kernel 处理一个 `64/32/16` query slice
- **THEN** 它必须至少在逻辑上拆成 `sign prepare phase` 和 `abs consume phase`
- **AND** 允许通过 kernel-local `signed_q_cache` 或等价 scratch 在两个阶段之间传递中间结果

#### Scenario: 第一阶段应限制 lane batching 的风险
- **WHEN** 第一阶段 kernel 引入 lane batching
- **THEN** 它应优先从 `2-lane` 或 `4-lane` batching 开始
- **AND** 不应在没有 profiling 证明前直接把全部 `8 lanes` 作为第一版默认宽度

### Requirement: Stage 2 compact kernel 应在更大范围的 kernel 重构前先清理 sign-path 的热循环开销
系统应优先清理 sign-path 的热循环开销，再考虑更大的 Stage 2 kernel 重构。compact kernel 必须先消除 sign chunk 的微拷贝开销，再减少同一组 loaded query slices 上重复的 per-lane flip-mask 工作，同时保持现有 compact-block layout 不变。

#### Scenario: sign chunk handling 避免热循环微拷贝
- **WHEN** compact kernel 在 Stage 2 batch 中处理 `64/32/16` sign chunks
- **THEN** 它必须避免在最热的 sign-path 循环里引入可避免的通用小拷贝工作
- **AND** 必须保持现有 compact-block layout 的 packed-sign 解释不变

#### Scenario: query flip work 减少但 kernel 语义不变
- **WHEN** compact kernel 对同一组 loaded query slices 在多个 valid lanes 上执行 flip
- **THEN** 它必须尽可能减少重复的 per-lane flip-mask 构造或等价重复 flip work
- **AND** 必须保持 reference compact kernel 的 raw inner-product 输出等价

### Requirement: Stage 2 compact kernel 仅在第一阶段证明收益后才允许进入 preload-time transcode
系统只有在第一阶段的 kernel 内 lane-batched scratch 证明确实能够改善 query-time kernel 热点之后，才允许引入 preload/load 阶段的 Stage2 并行友好视图。第二阶段不得绕过第一阶段直接落地。

#### Scenario: 第二阶段以前必须先完成第一阶段验证
- **WHEN** 系统准备引入 preload-time transcode
- **THEN** 必须先有第一阶段的 profiling 证据表明 lane-batched kernel 至少改善了 `stage2_kernel_sign_flip` 或 `stage2_kernel_abs_fma`
- **AND** 必须已有低开销 benchmark 结果表明 query-time 热路径没有因为第一阶段而退化

#### Scenario: preload-time transcode 不改变盘上格式
- **WHEN** 第二阶段构建 parallel-friendly Stage2 view
- **THEN** 它必须基于当前磁盘中的 compact-block 布局做加载后转码
- **AND** 不得要求重建索引或改变盘上格式

#### Scenario: 第二阶段的 resident view 应服务于 lane-batched kernel
- **WHEN** 第二阶段构建并行友好的 Stage2 view
- **THEN** 该 view 的组织必须优先服务于 query-time 的 lane-batched kernel 消费
- **AND** 其核心目的应是减少 query-time 的 layout decode、slice extraction 或等价重排工作
- **AND** 它必须首先服务于同一套 `sign prepare boundary` / `abs consume boundary`

#### Scenario: 第二阶段原型应暴露 query-time 与 load-time 的真实权衡
- **WHEN** parallel-friendly resident view 被构建并用于 query-time
- **THEN** 系统必须能够同时观测 query-time 的 `probe_stage2` 收益与 preload/load-time 的 `view_build_ms`、`view_bytes`
- **AND** 不得仅凭 query-time 的微小改善就默认认为第二阶段已收敛

### Requirement: Stage 2 compact kernel 应将 abs unpack/convert 和 bias accumulation 作为独立的后续优化轴
系统应将 magnitude consumption 视为 sign-path 清理之后的独立优化轴。compact kernel 必须允许 `LoadAbsMagnitude16Avx512()` 的消费链，以及周围的 abs/FMA 与 bias 路径，在不改变 compact storage layout 或 classification semantics 的前提下被收紧。

#### Scenario: abs/FMA 优化保持 layout 兼容
- **WHEN** compact kernel 减少 abs unpack/convert 和 bias/FMA 开销
- **THEN** 它必须继续直接消费现有的 compact-block ex-code layout
- **AND** 不能要求 layout migration 或 index rebuild

#### Scenario: profiling 仍足以验证 sign-first 和 abs/FMA 的优化
- **WHEN** sign-path 和 abs/FMA 优化在不同 pass 中应用
- **THEN** fine-grained profiling 必须仍然能够显示 `stage2_kernel_sign_flip` 或 `stage2_kernel_abs_fma` 是否下降
- **AND** kernel 子字段必须仍然能够回收到总 Stage 2 kernel 归因

### Requirement: Stage 2 compact kernel 应把 abs unpack/convert 视为首要的 abs 侧优化边界
系统应将 `LoadAbsMagnitude16Avx512()` 和 16B abs unpack/convert 链视为首要的 abs-side 优化边界。实现必须允许 abs 侧在不改变 compact-block layout 的前提下优化，并且必须继续把 bias accumulation 作为同一条热路径的一部分保留归因。

#### Scenario: abs unpack/convert 工作仍然是可测量的热路径
- **WHEN** compact kernel 在 `64/32/16` 路径中处理重复的 `16B` abs chunks
- **THEN** 实现必须继续暴露 abs/FMA 子步骤，以便独立测量其成本
- **AND** abs 工作必须与 reference compact kernel 保持等价

#### Scenario: bias accumulation 可以在不改变语义的情况下被重新审视
- **WHEN** abs 侧优化 pass 检查 bias accumulation 链
- **THEN** 实现必须保持现有 raw inner-product 语义
- **AND** 任何重排都必须保持当前 classification 结果不变

### Requirement: 第二阶段若引入 parallel-friendly resident view，应显式记录构建成本与内存占用
系统如果进入第二阶段并引入 preload-time transcode，则必须把 view 构建成本与额外内存占用作为一等观测量，而不能只报告 query-time 收益。

#### Scenario: 第二阶段必须保留构建成本观测
- **WHEN** parallel-friendly Stage2 view 在 preload/load 阶段被构建
- **THEN** 系统必须能够单独观测该 view 的构建时间
- **AND** 必须能够单独观测该 view 的额外内存占用或字节规模

#### Scenario: 第二阶段的收益判断应区分 query-time 与 load-time
- **WHEN** 第二阶段的方案被评估
- **THEN** 系统必须能够区分 query-time kernel 节省和 load-time transcode 新增成本
- **AND** 不得仅凭 query-time 降低就默认认为第二阶段方案成立

### Requirement: 第三阶段应在 parallel-friendly resident view 上实现 2-lane 注册驻留 batching
系统在第三阶段必须基于第二阶段构建出的 parallel-friendly resident view 实现 query-time 的 `2-lane` batching。第三阶段不得回退到第一阶段失败过的显式大 scratch 方案，而应把中间态限制在寄存器驻留或极小局部状态中。

#### Scenario: 第三阶段必须继续沿用稳定边界
- **WHEN** 第三阶段 batched kernel 被实现
- **THEN** 它必须继续围绕 `sign prepare boundary` 和 `abs consume boundary` 组织计算
- **AND** 不能重新把 sign 和 abs/FMA 混回单条不可分辨的 lane-local 热链

#### Scenario: 第三阶段必须优先尝试 2-lane
- **WHEN** 第三阶段开始引入 lane batching
- **THEN** 它必须先以 `2-lane` 作为默认批宽
- **AND** 只有在 profiling 证明收益明确时，才允许放大到 `4-lane`

#### Scenario: 第三阶段不得回到大 scratch 物化
- **WHEN** 第三阶段实现 lane batching
- **THEN** 它不能再使用第一阶段那种 query-time 显式物化大块 `signed_q_cache` 的方式
- **AND** 中间态必须限制在寄存器驻留或极小局部缓存中

#### Scenario: 第三阶段的收益仍需可归因
- **WHEN** 第三阶段 batched kernel 被测量
- **THEN** profiling 必须仍然能够区分 `stage2_kernel_sign_flip` 和 `stage2_kernel_abs_fma`
- **AND** 仍然必须能回收到总 Stage 2 kernel 归因

#### Scenario: 第三阶段实现顺序应先做 sign 共享粒度，再做 sign+abs 共享调度
- **WHEN** 第三阶段进入实现
- **THEN** 系统应先执行只重排 `sign prepare` 共享粒度的方案 A
- **AND** 只有在方案 A 表明共享粒度重排有效之后，才允许进入把 `sign + abs` 一起纳入 lane 共享调度的方案 B
- **AND** 不应在没有方案 A 证据时直接放大到 `4-lane`
