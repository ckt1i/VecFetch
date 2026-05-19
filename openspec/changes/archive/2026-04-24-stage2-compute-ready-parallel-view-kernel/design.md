## Context

当前版本已经完成了 resident preload 阶段的 Stage2 数据转码，但转码结果本质上仍然只是：

```text
[block][slice][lane][16B abs]
[block][slice][lane][16b sign]
```

这让输入地址更规整，但没有改变 query-time kernel 的核心执行模型。当前 [IPExRaBitQBatchPackedSignParallelCompact](/home/zcq/VDB/VectorRetrival/src/simd/ip_exrabitq.cpp:566) 仍然按 lane 独立执行：

```text
for slice:
  for lane:
    flip q
    load abs
    unpack/convert
    fmadd
```

因此，第二阶段现有实现失败的根因不是“view 没有转码”，而是“view 不是 compute-ready，kernel 也没有真正 lane-batched”。要继续推进版本二，必须同时升级 resident view 和 query-time kernel。

## Goals / Non-Goals

**Goals:**
- 把 resident Stage2 view 从“slice-friendly”升级为“compute-ready”。
- 在 resident/preload 阶段构建 `sign_pack` 和 `abs_pack`，只前移 query-independent 的准备工作。
- 在 query-time 引入轻量 `sign_ctx`，把 `sign_pack + q_slice` 转成 batch 可消费的 sign 上下文。
- 让新 kernel 按 lane batch 执行 `sign prepare -> sign apply -> abs consume`，不再维持 lane-local 主循环。
- 保持当前磁盘 compact blocked 存储格式、Stage2 分类语义和 recall 语义不变。

**Non-Goals:**
- 不修改磁盘上的 ExRaBitQ 存储格式。
- 不直接扩展到 8-lane；第一版只要求实现明确的 lane-batch kernel 骨架。
- 不重写 Stage2 collect/scatter 编排。
- 不在 preload 阶段生成 query-dependent 的 `signed_q` 或 `bias`。

## Decisions

### 决策 1：resident 侧只前移 query-independent 的前体表示

resident/preload 阶段必须只做不会依赖 query 的转换，因此它应构建：

- `sign_pack`
- `abs_pack`

而不应构建：

- `sign_ctx`
- `signed_q`
- `bias partial`

原因：
- `sign_ctx` 依赖当前 `q_slice`
- `signed_q` 与 `bias` 完全是 query-dependent
- 如果把这些东西前移，就会引入错误语义或过高内存代价

备选方案：
- preload 直接构建 `signed_q`。拒绝，因为它依赖 query，不成立。
- resident 仍保留 `uint16_t sign_words` / `uint8_t abs_slices` 的轻量视图。拒绝，因为这只改善地址组织，不足以支持 shared sign prepare。

### 决策 2：`sign_pack` 必须是 batch-friendly，而不是原始 sign word 的简单切片

`sign_pack` 的目标不是“把 16-bit sign word 存下来”，而是“为 query-time 低成本生成 `sign_ctx` 提供前体”。第一版推荐字段：

```text
SignPack
  lane_words[4]
  active_lane_mask
  bitplane[16]
```

其中：
- `lane_words[4]`：保留每个 lane 的原始 sign pattern，便于 fallback / debug
- `active_lane_mask`：标记这个 batch 里有效 lane
- `bitplane[16]`：第 k 个 dim 上各 lane 的 sign 分布

`bitplane` 的价值是把 resident 表示从“按 lane 存 sign”转换成“按 dim 存 lane sign 分布”，这更接近 query-time SIMD 的消费方向。

备选方案：
- 只存 `lane_words[4]`。拒绝，因为 query-time 仍要逐 lane 解释 sign，不足以支持 shared sign prepare。

### 决策 3：`abs_pack` 必须以 batch consume 为目标设计

`abs_pack` 的第一版至少应包含：

```text
AbsPack
  lane_bytes[4][16]
  active_lane_mask
```

可选扩展字段：

```text
  by_dim[16][4]
```

其中：
- `lane_bytes[4][16]`：为每个 lane 保留连续的 16B magnitude bytes
- `active_lane_mask`：配合 sign_pack，避免 query-time 反复判断 valid lane
- `by_dim[16][4]`：为更激进的 lane-batched unpack/consume 留接口

第一版可以先不强制 `by_dim` 落地，但 resident view 结构必须允许后续扩展到这个方向。

备选方案：
- 继续使用当前 `abs_slices` 指针数组。拒绝，因为它仍然让 query-time kernel 逐 lane 组织 unpack/convert。

### 决策 4：query-time kernel 必须分成三段

新 kernel 必须按以下三个 phase 组织，而不是继续写成 `for lane: flip -> abs -> fmadd`：

```text
Phase A: sign prepare
  q_slice + sign_pack -> sign_ctx

Phase B: sign apply
  sign_ctx + q_slice -> signed state + bias partial

Phase C: abs consume
  abs_pack + signed state -> dot partial
```

其中：
- `sign_ctx` 是 query-time 的轻量上下文
- `signed state` 应尽量寄存器驻留，不得回到第一阶段的大 scratch 物化

第一版推荐 `sign_ctx` 形态：

```text
SignCtx
  q_base
  sign_mask[4]
```

这样：
- preload 不需要处理 query-dependent 内容
- query-time 不需要把中间结果落成大 scratch
- kernel 可以真正先准备 sign 上下文，再批量消费 abs

备选方案：
- 继续把 `signed_q` 直接物化到 scratch。拒绝，因为此前已经证明 store/reload 成本过高。
- 仍按 lane 逐个做 `FlipQuery16PackedChunkAvx512`。拒绝，因为这正是当前 lane-local 模型无收益的根因。

### 决策 5：lane batch 是强制要求，不是可选优化

这个 change 的核心就是要求 query-time kernel 从 lane-local 模型切换到 lane-batched 模型。实现上可以先从 `2-lane` 开始，再扩大到 `4-lane`，但行为要求必须是：

- resident side 输出 batch-ready 的 `sign_pack/abs_pack`
- query-time kernel 以 batch 为单位构建 `sign_ctx`
- query-time kernel 以 batch 为单位进入 `sign apply` 和 `abs consume`

如果最终实现仍然是：

```text
for lane:
  ...
```

只是换了更规整的地址布局，则该 change 不算达成。

## Risks / Trade-offs

- [Risk] resident view 元数据更重，可能进一步增加 preload 时间和常驻内存。  
  → Mitigation：第一版只引入最小必要字段，继续记录 `parallel_view_build_ms` 和 `parallel_view_bytes`。

- [Risk] `bitplane` / batch sign 组织可能引入复杂的 bit 操作和寄存器压力。  
  → Mitigation：先固定较小 batch 宽度，优先建立清晰的 `sign_pack -> sign_ctx` 路径，再决定是否扩大。

- [Risk] 如果 kernel 虽然分成 phase，但仍然逐 lane 独立 apply / consume，则不会有真实收益。  
  → Mitigation：在设计和任务层面把“按 batch 处理 lane”列为硬性要求，并以 `stage2_kernel_sign_flip` / `stage2_kernel_abs_fma` 的下降作为验收门槛。

- [Risk] `AbsPack` 第一版如果过于保守，仍然只会优化地址组织，无法减少 unpack/convert。  
  → Mitigation：结构上预留 `by_dim` 一类字段，允许后续继续推进真正的 batch consume。

- [Risk] 新 resident view 与旧 compact path 双轨共存会增加维护复杂度。  
  → Mitigation：保留 fallback，但把 compute-ready path 明确为唯一目标路径，避免继续扩展旧的 parallel view 变体。
