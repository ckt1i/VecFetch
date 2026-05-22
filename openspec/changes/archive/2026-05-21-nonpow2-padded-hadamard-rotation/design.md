## Context

MSMARCO `768` 维工作点上的 perf 已经表明，在线查询的主要热点不再是 Stage2 kernel，而是 `PrepareQueryInto` 与 `RotationMatrix::Apply`。当前实现只在维度是 2 的幂时启用 Hadamard 旋转；对 `768` 维这类非 2 的幂输入，系统退回随机旋转的标量矩阵乘路径，导致 query prepare 保持较高的 `O(D^2)` 成本。

现有系统已经具备两块有利基础：

- Hadamard query-once 路径已经存在，且 build/query 两侧都支持 pre-rotated centroid/query 的执行模式。
- benchmark 已经能输出 prepare 子项与 probe 子项，适合做 baseline 与候选方案的同口径对照。

这次 change 的目标不是直接替换默认路径，而是引入一条受控的实验性路径：对非 2 的幂维度做 zero-pad 到下一档 2 的幂，然后复用现有 Hadamard 旋转与 pre-rotated query 流程，评估它是否在 MSMARCO 这类大规模数据集上优于当前随机旋转标量路径。

## Goals / Non-Goals

**Goals:**
- 为非 2 的幂维度引入 `logical_dim` / `effective_dim` 双维度模型，使 build 与 query 可以在保持原始输入语义的同时，对 padding 后的向量使用 Hadamard 旋转。
- 让索引元数据、rotated centroid artifact 和 reopen 路径完整表达 padded-Hadamard 的来源与执行契约。
- 支持两个正式对照组：
  - baseline `logical_dim` random rotation
  - padded `effective_dim` Hadamard
- 在 microbench 和 full E2E 下都输出足够细的 prepare/rotation 归因字段，用于判断“更快的旋转”是否被“更宽的 code / scan 线性成本”抵消。

**Non-Goals:**
- 不把 padded-Hadamard 直接设成默认路径。
- 不在本 change 内解决所有 random rotation 的 SIMD 优化问题。
- 不修改 CRC、submit-batch、Stage2 kernel 等与本实验无直接关系的主路径逻辑。
- 不重做 benchmark 输入系统或数据适配系统。

## Decisions

### Decision: 用 `logical_dim` / `effective_dim` 显式建模 padded 维度
索引与查询必须同时知道原始维度和实际参与旋转/编码的维度。设计上将：

- `logical_dim` 定义为原始 embedding 维度，例如 MSMARCO 的 `768`
- `effective_dim` 定义为编码和旋转所用维度；baseline 下等于 `logical_dim`，padded-Hadamard 下等于 `next_power_of_two(logical_dim)`，例如 `1024`

这样可以避免把“padding 后的实验路径”伪装成原始数据格式，方便 reopen、benchmark 和结果分析时回溯。

**Alternative considered:** 直接覆盖原始 `dim`  
这会让 index metadata、benchmark 输出和 recall 归因都变得含糊，不利于实验比较，因此不采用。

### Decision: 仅使用 zero-padding，不引入更复杂的投影或 learned expansion
候选方案只允许在尾部补零，使 padding 行为可解释、可复现、可与原始 L2 几何关系直接对应。所有 base vector、query 和 centroid 都使用同一 padding 规则。

**Alternative considered:** learned projection / random expansion  
虽然可能更灵活，但会把这次实验从“旋转/prepare 优化”扩展成“新量化前处理方法”，超出本 change 范围。

### Decision: build 与 query 两侧同时切换到 padded-Hadamard 才视为合法候选路径
不能只在 query 侧 pad 后做 Hadamard。合法候选路径必须保证：

- build 时按 `effective_dim` 生成量化 code
- build 时按 `effective_dim` 生成并持久化 rotated centroids
- query 时按同样的 `effective_dim` padding 并 query-once 旋转
- probe 时复用 `PrepareQueryRotatedInto`

这样才能让 prepare 路径和 distance-estimation 语义一致。

**Alternative considered:** 仅在 query 侧 pad + rotate  
会破坏 build/query 对称性，生成不一致的 residual 语义，因此不采用。

### Decision: 实验分为 microbench gate 和 full E2E gate 两层
这条方案的主要收益假设是“更快的 rotation/prepare”，而主要代价是假设“更宽维度导致后续线性项增加”。因此实验必须分两层：

- microbench：先比较 prepare/rotation 自身是否明显更快
- full E2E：再比较总查询延迟与 recall 是否整体受益

只有 microbench 明显有利，才值得进一步推进或默认启用。

**Alternative considered:** 直接只看 full E2E  
这样很难区分“rotation 变快”和“后续 scan 变慢”的抵消效应，不利于决策。

### Decision: benchmark 输出必须显式暴露 padding/rotation mode 与 prepare 子项
为保证实验结果可解释，benchmark 输出必须显式记录：

- `logical_dim`
- `effective_dim`
- `padding_mode`
- `rotation_mode`
- `prepare_rotation_ms`
- `prepare_quant_lut_ms`

如果没有这些字段，后续结果分析无法区分性能变化来自 padding、Hadamard、还是其他准备步骤。

## Risks / Trade-offs

- [维度膨胀增加线性成本] → `768 -> 1024` 会增大 code 长度、LUT 构建和部分扫描成本；通过 microbench + full E2E 双 gate 判断收益是否被抵消。
- [recall 变化] → zero-padding 不改变原始向量内容，但 rotation 与量化维度变化会改变误差分布；通过固定 operating point 下的 recall 对照控制风险。
- [索引体积上涨] → `effective_dim` 增长会扩大索引和 rotated centroid artifact；benchmark 必须同步记录 index size。
- [实验路径污染默认行为] → padded-Hadamard 只作为显式配置开启，默认路径保持现状。
- [实现扩散到过多模块] → 本 change 只涉及 build metadata、query prepare/rotation 路径和 benchmark/实验脚本，不顺带修改其他优化线。
