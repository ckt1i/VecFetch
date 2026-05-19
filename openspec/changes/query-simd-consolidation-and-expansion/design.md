## Context

当前查询热路径已经完成一轮 `submit-batch` 与 Stage2 collect/scatter 精修，固定 probe 主口径大约稳定在 `0.49ms`，CRC early-stop 主口径稳定在 `1.0ms` 左右。继续向下压时，主要剩余成本不再集中在 Stage2 kernel 本身，而是落在几类更分散的 CPU 骨架上：

- 地址解码与地址批处理；
- rerank 的精确距离与候选后处理；
- coarse top-n 的小规模选择；
- query loop 内仍存在的少量 SIMD 逻辑散落在业务模块中，破坏目录边界。

同时，代码结构上已经出现一个明确问题：部分 SIMD 逻辑直接写在 `src/index/cluster_prober.cpp`、`src/query/*` 等业务文件中。继续沿用这种方式，会让下一轮 SIMD 扩展同时放大两个成本：

1. 业务逻辑和 ISA 细节越来越耦合；
2. benchmark/fallback 变体越来越难以管理。

因此这轮设计必须同时回答两个问题：

```text
1. 继续在哪里做 SIMD？
2. 新老 SIMD 代码应该放在哪里？
```

本轮约束：

- 不改索引格式；
- 不新增 resident layout 前置条件；
- 不改变 recall、funnel 语义、最终排序语义；
- 所有新增 SIMD 实现必须默认进入 `src/simd` / `include/vdb/simd`；
- 已有散落在业务层的 SIMD 逻辑也要迁移到 `simd` 目录，只在业务层保留调用与 feature gate。

## Goals / Non-Goals

**Goals:**
- 为 query 路径中仍然值得继续 SIMD 化的部分建立新的实现入口，并把“代码归位”和“更强 specialization”拆成两个层次：
  - address decode batch 化
  - rerank batched exact distance
  - coarse top-n 小规模选择器
  - 已有 Stage2 classify/辅助 SIMD 逻辑的目录收拢
- 建立统一的 SIMD 代码组织规范：
  - 业务模块只负责准备输入、选择路径、消费输出
  - ISA/向量化细节集中在 `simd` 目录
- 为每个 SIMD 点保留可关闭的 fallback
- 让 benchmark 能在 full E2E 与 query-only 下对比 current / fallback / SIMD-on 结果

**Non-Goals:**
- 不重新开启 Stage2 compute-ready resident layout 路线
- 不继续把 online submit tuning 作为默认主线优化
- 不要求所有 query 路径都改成 SIMD，只覆盖当前有明确收益空间和清晰边界的点
- 不在本轮引入新的第三方 SIMD 抽象库
- 不要求所有新增 SIMD 能力在第一阶段就达到最终最强 ISA 版本；Phase 1 可以先使用保守但结构正确的实现

## Decisions

### 1. 所有 SIMD 实现必须收敛到 `src/simd` 与 `include/vdb/simd`

本轮最核心的组织决策是：

```text
业务层: 组装输入 / 选择路径 / 消费结果
SIMD层: ISA 分支 / 向量寄存器 / 批量 kernel
```

因此：

- 新增 SIMD 代码 SHALL 放在 `src/simd/*.cpp` 与 `include/vdb/simd/*.h`
- 已有直接写在业务模块中的 SIMD 逻辑 SHALL 被迁移到 `simd` 目录
- 业务模块不得继续内嵌新的 AVX/ISA 具体实现，最多保留：
  - feature gate
  - scratch 准备
  - 对 `simd::*` API 的调用

这样做的理由：

- 目录职责清晰，后续继续扩展不会污染 `src/index` / `src/query`
- fallback 与 benchmark 对照更容易做
- SIMD 核心代码更容易被单独 benchmark / review

备选方案：
- 保持“就地内嵌 SIMD”更省改动，但会继续恶化目录结构
- 为每个模块各自维护一个局部 SIMD helper，最终仍会形成分散重复

### 2. query SIMD 工作拆成两类：一次性 consolidation 与两阶段 runtime specialization

本轮涉及的 SIMD 点分散在不同 query 子路径上，而且风险不同。因此本轮明确拆成两类：

```text
Track 1: Consolidation
  - 现有 SIMD helper 迁移到 simd 目录
  - 一次完成

Track 2: New Runtime Paths
  - Phase 1: interface + conservative implementation
  - Phase 2: specialization + stronger SIMD path
```

其中：

- Track 1 包括：
  - `Stage2ClassifyBatch`
  - `Stage1SafeInMask`
  - 同类散落在业务层但已成形的 SIMD helper
- Track 2 包括：
  - `simd/address_decode.*`
  - `simd/rerank_distance.*`
  - `simd/coarse_select.*`

每个 Track 2 能力都按“独立 kernel + feature gate”组织，而不是一次性大重构：

- `simd/address_decode.*`
- `simd/rerank_distance.*`
- `simd/coarse_select.*`
- `simd/stage2_classify.*` 或等价命名

Phase 1 的每个能力都满足：
- 独立头文件与源文件
- 明确的 scalar fallback
- 业务层通过 config / feature gate 显式选择

Phase 2 才允许：
- 追加更强的 AVX2/AVX-512 路径
- 追加更激进的小 top-n specialized selector
- 在不改变业务接口的前提下替换 `simd/*.cpp` 内部实现

理由：
- 便于逐点启用和回归
- 便于 query-only / full E2E 做局部归因
- 降低一次性改太多的风险

### 3. Address decode 必须按两阶段推进

地址解码当前虽然暴露了 `DecodeAddressBatch()` 接口，但本质仍接近逐元素处理。  
本轮设计要求：

- Phase 1:
  - 地址批解码的核心实现迁移到 `simd/address_decode.*`
  - 先提供语义稳定的 batch 接口和保守实现，可以是 branchless / table-driven batch 版本
- Phase 2:
  - 在不改变接口的前提下追加更强的 AVX2/AVX-512 specialization
- 业务层的 `ParsedCluster::DecodeAddressBatch()` 只保留：
  - 输入校验
  - 选择 SIMD / scalar path
  - 输出回填

理由：
- 这块既可能做 SIMD，也可能先做 branchless / table-driven 优化
- 无论具体实现如何，都不应该继续留在 query 模块内部膨胀

### 4. Rerank batched distance 采用“先 kernel，后更强 specialization”的两阶段方案

`RerankConsumer` 后续最值得 SIMD 化的点，是 exact distance 本身，而不是一开始就全面重写 consumer。

因此本轮只要求：
- Phase 1:
  - 提供 `1 query × N candidates` 的 batched exact distance kernel
  - kernel 进入 `simd/rerank_distance.*`
  - `RerankConsumer` 只改成消费该 batched kernel，不重写生命周期
- Phase 2:
  - 在同一接口下追加更强的 batched distance specialization
  - 可以再评估是否需要更积极的 prefetch / pack 策略，但不把 consumer 重构绑定为本 change 的必做项

这样做的原因：
- 可以先拿到稳定、低风险的 CPU 收益
- 避免把 rerank 的数据结构重写和 SIMD kernel 改造耦合在一起

### 5. Coarse top-n 采用“先 specialized wrapper，后更强 small-topn”的两阶段方案

当前 coarse score 已经 SIMD 化，但 top-n 选择仍是通用 `nth_element + sort`。  
本轮设计要求新增一个针对小 `nprobe` 的专门选择器：

- Phase 1:
  - 先提供 small-topn 的独立 `simd/coarse_select.*` 接口
  - 可以先采用语义稳的 specialized scalar / small-batch 实现
  - 优先服务 `nprobe=64` / `256` 这类主工作点
- Phase 2:
  - 再在同一接口下追加更激进的 vectorized block select / merge specialization
- 业务层 `FindNearestClusters()` 始终保留 fallback 到当前通用实现

理由：
- 这是 query 每次都要付的固定成本
- 不改 recall 语义
- 很适合以 small-topk kernel 的方式单独演进

### 6. Stage2 已有 SIMD 逻辑属于 consolidation，一次性迁移

本轮明确要求把以下逻辑从 `cluster_prober.cpp` 中迁移出去：

- `Stage2ClassifyBatch`
- 后续若继续演进的 classify / compress helpers
- 任何新增 AVX/ISA 相关实现

迁移后结构应接近：

```text
cluster_prober.cpp
  ├─ collect / block scheduling / stats
  └─ 调用 simd::Stage2ClassifyBatch(...)

src/simd/stage2_classify.cpp
  ├─ AVX2/AVX512 path
  └─ scalar fallback
```

理由：
- 当前业务文件已经重新出现 SIMD 细节
- 不迁移的话，这轮“目录收拢”目标就不成立

### 7. Benchmark 需要同时覆盖 Phase 1 和 Phase 2 的显式维度

benchmark 不仅要看时间，还要能回答“哪个 SIMD 能力打开了”。  
因此导出字段必须至少区分：

- `stage2_block_first`
- `stage2_batch_classify`
- `submit_online`
- 后续新增的：
  - `address_decode_simd`
  - `rerank_batched_distance`
  - `coarse_select_simd`

并且：
- query-only 下要保留对应阶段的时间字段
- full E2E 下要验证 recall 不变
- 若某个能力进入 Phase 2，还应能区分：
  - `*_phase1`
  - `*_phase2`
  或等价的 conservative/specialized 运行模式字段

## Risks / Trade-offs

- `[SIMD 迁移过程引入行为偏差]` → 每个 kernel 先保留 scalar fallback，并用 query-only + full E2E 双口径对比 current/fallback。
- `[目录收拢带来大面积文件改动]` → 先完成 consolidation，再进入新增 runtime path 的 Phase 1，避免一次性同时改功能和结构。
- `[coarse/top-n 专门化收益有限]` → 以 feature gate 方式引入，若收益低于噪声，可保留代码但默认关闭。
- `[rerank batched distance 只解决部分问题]` → 本轮明确只先解决 exact distance kernel，不承诺同时完成 rerank 生命周期重构。
- `[address decode 不一定适合纯 SIMD]` → capability 定义允许 branchless/table-driven 实现，但代码归属仍必须进入 `simd` 目录。
- `[业务层调用接口增多]` → 通过统一命名和 feature gate 减少调用混乱，例如 `enable_*_simd` 或 `simd::*` API 分层。
- `[Phase 2 收益不稳定]` → 将 Phase 2 明确设为建立在 Phase 1 correctness 与 benchmark 稳定之上的后续步骤，而不是本 change 一开始就必须兑现的强承诺。

## Migration Plan

1. 先建立 capability 对应的 spec，并确认 benchmark 维度与目录规范。
2. 先迁移现有散落在业务层中的 SIMD 逻辑到 `simd` 目录，不改行为。
3. 对新增 runtime path 先完成 Phase 1：
   - address decode interface + conservative batch path
   - rerank batched distance interface + initial kernel
   - coarse small-topn interface + specialized wrapper
4. 每个 Phase 1 点都按同一顺序验证：
   - scalar fallback correctness
   - query-only perf
   - full E2E recall + avg_query
5. 只有当对应能力在 Phase 1 下 correctness 稳定、benchmark 可归因时，才进入 Phase 2 specialization。
6. 在线 submit tuning 保持已实现但默认关闭，不把它作为本轮收益目标。

## Implementation Priority Table

| Priority | Work Item | Why First | Entry Criteria | Exit Criteria |
|---|---|---|---|---|
| P1 | `Stage2ClassifyBatch` / `Stage1SafeInMask` consolidation | 已有实现、风险低、目录收益立刻可见 | 当前 helper 已在业务层工作正常 | helper 完整迁入 `simd` 目录，业务层仅保留调用 |
| P2 | `address_decode` Phase 1 | query 固定成本、语义边界清楚、适合先做保守 batch path | 已有 batch 接口可挂接 | Phase 1 接口稳定，fallback 可回退，decoded address 与旧路径一致 |
| P2 | `rerank_distance` Phase 1 | 不需要先重构 rerank 生命周期，收益更稳 | `RerankConsumer` 可按小批量调用新 kernel | batched exact distance 接通，top-k 语义不变 |
| P3 | `coarse_select` Phase 1 | 值得做，但更依赖工作点与 tie-breaking | 已确认主工作点仍为小 `nprobe` | small-topn wrapper 路径稳定，可与参考路径逐项比较 |
| P4 | `coarse_select` Phase 2 | 当前唯一进入 Phase 2 的能力 | Phase 1 correctness 与 benchmark 稳定 | Phase 2 已实现并在低负载下复核，但 benchmark 仍未证明默认开启收益，保持默认关闭 |
| P5 | `address_decode` / `rerank_distance` Phase 2 | 目前没有明显收益信号，先保留 Phase 1 | 仍以 Phase 1 为默认实现 | 仅在后续证据充分时再推进对应 specialization |

优先级的含义不是“后面的不做”，而是：

```text
先收口现有 SIMD
   ↓
先建立新增能力的 Phase 1
   ↓
确认 correctness / fallback / benchmark 都稳定
   ↓
再决定是否继续做 Phase 2
```

这保证了后续实现不会把“目录整理”和“激进性能尝试”混成同一批风险。

## Open Questions

- address decode 的 Phase 2 最终更适合纯 SIMD，还是 branchless + small lookup table 的混合实现？
- coarse select 的 Phase 2 是否只支持当前主工作点 `64/256`，还是保留更通用接口？
- rerank batched distance 的 Phase 2 是否需要同时提供 AVX2 与 AVX-512 双实现，还是先以 AVX2 为主？
- Stage2 classify 迁移到 `simd` 后，是否顺手把压缩写回辅助逻辑也一起迁走，还是保留在业务层？
