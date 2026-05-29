## Context

当前 `ProbeCluster` 的 Stage2 compact v11 路径会先在 `ClusterProber::Probe` 中收集 Stage1 uncertain lane，形成 `lane_mask`，随后调用 ExRaBitQ compact SIMD kernel 计算 IP。现有 kernel 使用 `valid_count` 遍历 block 内全部有效 lane，scatter/classify 阶段再用 `lane_mask` 过滤真正需要的 lane。

这意味着当一个 Stage2 block 中 uncertain lane 较稀疏时，kernel 仍然计算了无关 lane。fine-grained timing 显示 Stage2 kernel 是 `ProbeCluster` 最大拆分项；perf 也显示 `IPExRaBitQBatchPackedSignParallelCompact` 是查询热路径之一。因此下一轮优化应优先减少 Stage2 实际计算量，而不是调整 epsilon/nprobe 等参数。

## Goals / Non-Goals

**Goals:**
- 让 compact v11 Stage2 kernel 支持 `lane_mask`，只计算 Stage1 uncertain lane。
- 保持现有 full-lane compact kernel 可用，作为 fallback、测试对照和非 mask 场景路径。
- 在 benchmark 输出中补充 Stage2 lane 密度/跳过 lane 统计，帮助判断收益来源。
- 保持 query 结果、recall、top-k 排序、CRC、I/O、rerank 行为不变。
- 验证真实 GT、`--skip-gt 0`、`--early-stop 0` 口径下的端到端收益。

**Non-Goals:**
- 不调整 epsilon、nprobe、two-level coarse routing、early-stop 等搜索参数。
- 不实现 768-only SIMD 特化。
- 不改索引格式，不新增持久化字段。
- 不重写 Stage1 FastScan、probe submit、rerank 或 I/O pipeline。
- 不把 fine-grained timing 默认打开。

## Decisions

### Decision 1: 新增 masked kernel，而不是修改原 kernel 签名

新增 `IPExRaBitQBatchPackedSignParallelCompactMasked(...)`，显式传入 `lane_mask`。原 `IPExRaBitQBatchPackedSignParallelCompact(...)` 保留。

理由：
- 原 kernel 仍可作为 full-lane fallback 和单元测试 oracle。
- 避免影响当前其他调用点或历史 benchmark。
- masked 路径可以围绕 sparse lane 做专门循环，不需要在 full-lane 路径中引入额外分支。

备选方案：
- 直接给原 kernel 增加可选 `lane_mask` 参数。缺点是调用语义变复杂，并可能让 full-lane 热路径承担额外分支。

### Decision 2: `ClusterProber::Probe` 只在 compact v11 parallel layout 下启用 masked kernel

当 `pc.exrabitq_storage_version >= 11` 且 `parallel_view.abs_slices/sign_words` 可用时，使用 masked kernel。否则继续走原路径。

理由：
- 当前热点来自 compact blocked v11 path。
- legacy layout 的 pointer-array path 和 scalar fallback 不应在本轮混改。
- 保持 unsupported 或非 parallel layout 的行为稳定。

### Decision 3: `lane_mask` 的输出布局保持与原 lane 编号一致

masked kernel 仍写入 `out_ip_raw[lane]`，其中 `lane` 是原 block-local lane id；未计算的 lane 不保证有效。scatter 继续使用 `block.lane_mask` 读取结果。

理由：
- `ClusterProber` 下游代码无需重排 lane。
- 避免引入 compacted lane 到 original lane 的映射数组。
- 更容易与 full-lane kernel 做逐 lane 数值对比。

### Decision 4: 统计 lane 密度，而不是输出新的 vec-only 子系统分析

新增统计建议：
- `stage2_masked_kernel_calls`
- `stage2_lanes_requested`
- `stage2_lanes_skipped`
- `stage2_lanes_total_valid`

benchmark 输出聚合平均值或总量，并保留现有字段。

理由：
- 这些字段直接解释 masked kernel 是否减少了实际 lane 计算。
- 不偏离当前实验主线，仍围绕端到端向量搜索与原始向量读取流程。

### Decision 5: SIMD 模板特化留到后续 change

本轮只做 mask-aware 计算量削减。若验证 masked kernel 有收益，再考虑独立 change 做 `n * 32` / `n * 64` 维度的通用模板特化。

理由：
- mask-aware 优化减少不必要计算，收益来源清晰。
- 模板特化会扩大 SIMD 代码面和测试矩阵，不应与本轮混合，避免归因不清。

## Risks / Trade-offs

- Masked kernel 数值与 full-lane kernel 不一致 -> 用单元测试对随机 `lane_mask`、不同 `valid_count`、512/768/1024 维做逐 lane 误差比较，阈值沿用现有 Stage2 kernel 容差。
- lane 很密时 masked kernel 分支/ctz 循环可能慢于 full-lane -> 在 `lane_mask == full_valid_mask` 时直接调用 full-lane kernel，或在 lane density 高于阈值时 fallback full-lane。
- AVX512 register pressure 变高 -> masked kernel 首版优先复用现有 lane-batch 思路，只跳过无关 lane，不同时引入模板展开。
- fine-grained timing 会放大 kernel 开销 -> 正式性能结论必须用 `--fine-grained-timing 0`，fine-grained 只用于诊断。
- 统计字段增加 JSON 内容 -> 保留旧字段名，新字段只追加，不改变已有 schema 消费方。
