## Context

最近的 SafeIn 工作在 `OverlapScheduler`、`bench_vector_search` 和 `bench_e2e` 中加入了一组探索性的 Dynamic SafeIn 矩阵。实验最终收敛到一个有用行为：使用查询过程中在线维护的 lower-bound top-k frontier 作为 SafeIn prefetch 阈值，并通过 deferred candidate buffer 延迟早期候选的最终读类型判断。

因此，最终 runtime 形态比当前代码更简单。当前分支仍包含多个未采用模式（`frontier_cap`、`frontier_delay`、`frontier_stable`、`frontier_scale`）、通过 `--dynamic-safein-scale` 实现的 lambda 插值、quality-gap gate、payload-only gating，以及对应的指标和测试。这些选项会增加查询路径理解成本，也让 benchmark 命令看起来比最终设计更可调。

## Goals / Non-Goals

**Goals:**

- 只保留 `static/off` 和已接受的 Dynamic SafeIn 模式。
- 将 `frontier_blend` 更名为 `frontier`。
- 固定 frontier 模式语义为 `T_q = F_lower`，删除 lambda/scale 插值。
- 保留 `defer_initial_clusters` 和 `defer_until_ready` 所需的 deferred candidate buffer 与 reclassification 机制。
- 删除无效 config 字段、CLI flags、JSON/CSV/log 字段、测试和关于废弃模式的脚本/文档。
- 保持 static SafeIn 行为和现有 Dynamic SafeOut 行为不变。

**Non-Goals:**

- 不新增 SafeIn 算法或新的 tuning sweep。
- 不重新引入 quality-aware early-start gate。
- 除保持当前已接受的 `frontier + defer4` 配置可用外，不引入数据集专用默认值。
- 不修改 index metadata、SafeIn d_k calibration 或 SafeOut calibration。

## Decisions

### Decision 1: 将 `frontier` 作为唯一动态模式名称

Runtime enum 和 CLI 只暴露：

- `static` / `off`：legacy global SafeIn threshold 行为。
- `frontier`：基于当前 lower-bound frontier 的 query-adaptive threshold。

应删除 `frontier_blend`，而不是保留为兼容 alias。这是一次 breaking cleanup，但可以避免长期歧义，因为实现不再对 lower/upper frontier 做 blend。

备选方案：保留 `frontier_blend` 作为兼容 alias。拒绝原因是该名称隐含已删除的 lambda 行为，会让旧实验命令看起来仍然有效。

### Decision 2: Frontier 阈值固定为 `F_lower`

动态模式使用：

```text
T_q = F_lower
```

其中 `F_lower` 是当前已观察候选中第 k 小的 lower-bound estimate；只有 frontier heap 已填满且 readiness 条件满足后才可用。

删除：

- `dynamic_safein_scale`
- `dynamic_safein_scale_cap_to_static`
- 任意 `F_lower + lambda * (F_upper - F_lower)` 插值逻辑
- scale/cap 相关测试与输出字段

备选方案：保留 lambda 但默认设为 `0.0`。拒绝原因是实验显示 lambda 会让 prefetch 更激进，且没有稳定的跨数据集收益；用户也明确希望删除该参数。

### Decision 3: 只保留 deferred frontier 操作所需的 readiness

最终设计保留：

- `dynamic_safein_stable_probes`
- `dynamic_safein_rel_tol`
- `dynamic_safein_abs_tol`
- `dynamic_safein_defer_initial_clusters`
- `dynamic_safein_defer_until_ready`
- `dynamic_safein_defer_max_candidates`

这些字段支持当前 `defer4` 机制，并避免 frontier 尚不可用时过早启动 SafeIn。`min_probes` 只有在实现中仍有正确性价值时才保留；否则应和其他未采用控制项一起删除。

删除：

- gap relative/absolute tolerance 字段
- gap readiness 逻辑与 stats
- payload-only mode
- 旧 mode-specific readiness 分支

### Decision 4: 保留 prefetch/read accounting，删除 mode-specific diagnostics

保留用于解释剩余算法的指标：

- vector-only / all / payload read counts
- SafeIn payload prefetched count
- remaining payload fetch count
- deferred candidates / flushes / SafeIn count
- dynamic SafeIn active/disabled cluster counts
- frontier/threshold averages 和 final values

删除只服务于废弃 gate 或造成虚假精确感的指标：

- gap samples / gap ready samples / gap average / final gap
- scale/cap config output
- payload-only config output
- 已废弃 mode names 的日志和 JSON 输出

Prefetch truth stats 只在 benchmark 能低成本提供 truth 时保留。`--skip-false-stats` 对大规模 MS MARCO 运行仍然有用，本 change 不删除它。

## Risks / Trade-offs

- 旧实验命令在删除 modes/flags 后会失败 -> 更新脚本/文档，并让 benchmark parse error 明确。
- 删除 `frontier_blend` alias 会造成短期迁移成本 -> 给出单一路径：将 `frontier_blend --dynamic-safein-scale 0.0` 替换为 `frontier`。
- 过度删除 diagnostics 可能掩盖回归 -> 保留 read counts、deferred counts、active/disabled counts、threshold/frontier summaries。
- readiness 简化可能改变精确 prefetch 时机 -> 用 COCO 和 MS MARCO smoke run 对照当前 `blend000_defer4` 行为。

## Migration Plan

1. 更新 runtime enum/config，只暴露 `Static` 和 `Frontier`。
2. 从 `bench_vector_search` 和 `bench_e2e` 删除废弃 CLI flags。
3. 将已接受命令从 `frontier_blend` 改为 `frontier`。
4. 更新测试，覆盖 static 行为、frontier 阈值 `T_q = F_lower`、deferred flush 行为。
5. 更新实验脚本/文档，或将旧实验日志标记为 historical。
6. 构建并运行 focused tests，再运行代表性的 COCO/MS MARCO smoke benchmarks。
