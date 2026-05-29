## Context

当前 Stage1 FastScan 路径在 `ClusterProber::Probe` 中按 32-vector block 执行：

```text
AccumulateBlock(packed_codes, lut) -> raw_accu[32]
FastScanDequantize(raw_accu, block_norms) -> dists[32]
FastScanSafeOutMask(dists, block_norms) -> so_mask
optional FastScanSafeInMask(dists, block_norms) -> safein_mask
CompactMaskToIndices(~so_mask)
```

在 `mask-aware-stage2-kernel` 后，Stage2 的无效 lane 计算已经被压低，`EstimateDistanceFastScan` 成为与 masked Stage2 同量级的热点。Stage1 当前主要问题不是参数，而是 dequantize 与 mask 计算拆成多个 pass，导致对 `dists[32]` 和 `block_norms[32]` 的重复访问和额外 SIMD loop。

同时，当前主线已经采用 `safe-boundary-error-frontier` 的区间式分类语义：

```text
候选下界: dist - error_bound
候选上界: dist + error_bound

SafeOut: dist - safeout_margin > safeout_frontier_upper
         等价于 dist > safeout_frontier_upper + safeout_margin

SafeIn:  dist + safein_margin < safein_d_k
         等价于 dist < safein_d_k - safein_margin
```

因此本 change 的融合目标只是在同一 pass 中计算距离和 mask，不改变当前 SafeIn/SafeOut 边界。

## Goals / Non-Goals

**Goals:**
- 新增 fused Stage1 API，在一次 SIMD pass 中完成 dequantize、SafeOut mask、可选 SafeIn mask。
- 保留旧三步路径作为测试 oracle 和 fallback。
- fused API 继续输出 `dists[32]`，避免影响 Stage2、candidate emit、CRC heap 和调试统计。
- fused API 必须对齐当前 `safeout_frontier_upper + margin` / `safein_d_k - margin` 单边区间语义。
- 保持 query 语义、recall、top-k 排序、CRC、I/O、rerank 行为不变。
- 用真实 GT、`--skip-gt 0`、`--early-stop 0` 做 before/after 验证。

**Non-Goals:**
- 不做 epsilon、nprobe、early-stop、two-level routing 等参数优化。
- 不实现 `n * 32` / `n * 64` 维度模板化；该方向留给后续独立 change。
- 不重写 `AccumulateBlock` 的 VPSHUFB 累加逻辑。
- 不改索引格式。
- 不把 fine-grained timing 默认打开。

## Decisions

### Decision 1: 新增 fused evaluate API，而不是改写旧 API

新增：

```cpp
struct FastScanStage1EvalResult {
    uint32_t safeout_mask = 0;
    uint32_t safein_mask = 0;
};

FastScanStage1Evaluate(
    raw_accu,
    block_norms,
    count,
    fs_shift,
    fs_width,
    sum_q,
    inv_sqrt_dim,
    norm_qc,
    norm_qc_sq,
    safeout_frontier_upper,
    safeout_margin_factor,
    safein_threshold_base,
    safein_margin_factor,
    enable_safein,
    out_dists,
    &result);
```

其中：

```text
safeout_margin_v = safeout_margin_factor * block_norms[v]
safein_margin_v  = safein_margin_factor  * block_norms[v]

result.safeout_mask[v] = dist[v] > safeout_frontier_upper + safeout_margin_v
result.safein_mask[v]  = dist[v] < safein_threshold_base - safein_margin_v
```

`safeout_frontier_upper` 是 `OverlapScheduler` 基于 CRC estimate heap 导出的保守 top-k 上界：heap 未满时为 `+inf`，heap 满时为 `kth_est_dist + max_error_in_est_heap`。fused API 不负责维护 heap，也不重新解释该 frontier。

理由：
- 旧 `FastScanDequantize`、`FastScanSafeOutMask`、`FastScanSafeInMask` 可继续用于测试对照。
- fused API 的输入输出清晰，便于单元测试逐 lane 对比。
- 不影响其他使用 `FastScanDequantize` 的非查询路径。

备选方案：
- 在 `RaBitQEstimator::EstimateDistanceFastScan` 内直接返回 masks。缺点是 `RaBitQEstimator` 会开始理解 SafeOut/SafeIn 阈值语义，职责边界变混乱。
- 在 fused API 中保留旧 `dynamic_d_k + 2 * margin` 公式。该方案与当前 `safe-boundary-error-frontier` 主线冲突，会改变 SafeOut/SafeIn 分布和 recall 风险，因此拒绝。

### Decision 2: `AccumulateBlock` 暂时不动

Stage1 主流程仍先调用 `AccumulateBlock` 得到 `raw_accu[32]`，再调用 fused evaluate。

理由：
- 本轮收益归因聚焦于 dequantize/mask pass 融合。
- `AccumulateBlock` 模板化属于更底层 SIMD 特化，风险和测试矩阵更大。
- 保留 `raw_accu` 边界便于用旧路径做 oracle。

### Decision 3: fused path 仍输出 `dists[32]`

虽然 SafeOut/SafeIn 可以在 fused pass 内直接得到，但 `dists[j]` 仍被后续代码用于：
- `est_dist_s1`
- Stage2 candidate entry
- candidate batch emit
- CRC estimate buffering

因此 fused API 必须写出 `out_dists`，但避免再用额外 pass 读取 `out_dists` 计算 masks。

### Decision 4: SafeIn 通过 `enable_safein` 控制

当 `enable_safein=false` 时，fused API 不计算 SafeIn threshold/mask，直接返回 `safein_mask=0`。

理由：
- 当前主要 benchmark 配置下 Stage1 SafeIn 常为关闭或贡献很小。
- 避免在热路径为未启用功能支付额外比较成本。

### Decision 5: fused path 不改变 candidate error-bound plumbing

`CandidateBatch.est_error` 和 CRC estimate heap 使用当前候选的 error bound 来维护 `safeout_frontier_upper`。fused Stage1 只负责产生 `dists`、SafeOut mask、SafeIn mask；`ClusterProber` 仍按当前逻辑为 surviving candidate 写入 `est_error`，通常是 `safeout_margin_s1`。

理由：
- 避免把 CRC heap 语义下沉到 SIMD fused API。
- 保持 `safe-boundary-error-frontier` 的 frontier 维护逻辑不变。
- fused path 与旧三步 path 的可比性更强，便于回归测试。

### Decision 6: 追加观测字段但保留旧字段

可追加：
- `stage1_fused_blocks`
- `stage1_fused_safeout_lanes`
- `stage1_fused_safein_lanes`

这些字段用于确认 fused path 被使用。旧 timing 字段和 JSON 字段必须保留。

## Risks / Trade-offs

- Fused path 与旧路径 mask 不一致 -> 用单元测试对 `dists`、SafeOut mask、SafeIn mask 做逐 lane 对比，覆盖 `count=1/7/16/31/32`、不同阈值、`enable_safein` 开关。
- 浮点比较边界导致分类差异 -> fused path 的公式、比较方向、`max(dist, 0)` 必须与旧 `FastScanDequantize` 后再 mask 的结果一致。
- 误用旧 `2 * margin` 公式 -> 会改变 SafeOut/SafeIn 分布并可能引入 false SafeOut；测试必须直接对比当前 `FastScanSafeOutMask` / `FastScanSafeInMask` oracle。
- `safeout_frontier_upper=+inf` 行为被破坏 -> heap 未满时不应产生 SafeOut；单元测试需要覆盖 infinity frontier。
- AVX2/scalar fallback 行为不一致 -> fused API 在 AVX512、AVX2、scalar 下都必须可用；如果首版只优化 AVX512，AVX2/scalar 仍应通过旧逻辑 fallback。
- 收益被 `AccumulateBlock` 主成本掩盖 -> 这是预期风险；若融合收益有限，下一轮再做 dim-block template dispatch。
- fine-grained timing 扰动热路径 -> 正式性能结论必须用 `--fine-grained-timing 0`，fine-grained 只用于诊断。
