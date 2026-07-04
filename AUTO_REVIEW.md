# Auto Review Loop: Budgeted Prefetch Pipeline

## Round 1 (2026-07-03T12:52:11+08:00)

### Scope

本轮目标是在保持最终 rerank budget 上限的前提下，验证更激进 probe/prefetch 是否能提升性能：

- 先跑 no-budget 上界测试，验证“当前 budget 推迟 I/O 导致 pipeline 收益受限”的判断。
- 再实现并测试方案B：保留最终 top-B rerank budget，增加 speculative raw-vector prefetch cache。
- 先在 Amazon ESCI 上做参数探索，再扩展到五个数据集。
- 最后与 no-pipeline 方案对比。

### Initial Code Finding

当前实现中，开启 `non_safeout_candidate_budget > 0` 后，SafeIn/Uncertain 产生的 read plan 会先进入 `budgeted_read_plan_heap_`；所有 cluster probe 结束后才在 `MaterializeBudgetedReadPlans()` 中转成真正的 I/O 请求。因此 budget 口径下的执行路径不是完整的 probe/read/rerank overlap。

### Reviewer Setup

本会话没有暴露 `mcp__codex__codex` / `mcp__codex__codex-reply` 工具。按照 auto-review-loop 的意图，本轮改用可用的多代理 reviewer 做独立审查，并将结果记录在本文件。

### Actions Started

- 创建实验目录：`/home/zcq/VDB/test/recordgate_budgeted_prefetch_pipeline_20260703`
- 准备启动独立 reviewer。
- 准备复用现有索引跑 Amazon no-budget 上界测试。

### Reviewer Feedback Summary

独立 reviewer 指出：

- no-budget 只能作为上界，因为它同时改变了读量、rerank 数和 payload 行为。
- online runner 需要输出 `candidate_budget_*`、`io_wait` 和方案B专项指标，才能支持后续判断。
- 方案B保持最终 rerank budget 的 invariant 基本正确，但投机上限会被早期后续淘汰的候选消耗。
- 初始 sweep 的小幅收益需要警惕顺序和 OS page cache 干扰。

### Actions Taken

- 实现 `--budgeted-prefetch-limit`。
- 实现 budget-preserved speculative raw-vector prefetch cache。
- 实现 cache hit owning-buffer 转交，减少二次 copy。
- 补充 online runner 诊断输出：candidate budget、unique fetch、io wait、prefetch 专项统计。
- 跑完 Amazon no-budget 上界、Amazon-only sweep、五数据集 sweep、warm paired repeat、selected pipeline vs no-pipeline 对比。

### Results

本轮完整结果见：

- `/home/zcq/VDB/test/recordgate_budgeted_prefetch_pipeline_20260703/RESULT_SUMMARY_CN.md`

核心结论：方案B保持 recall 和最终 rerank budget，但不是通用加速。它对 ImageNet1K/VoxCeleb2 的部分 high-drain workload 有收益，其中 VoxCeleb2 top100 在 selected pipeline 对比中约快 34.5%；对 Amazon/MSMARCO/COCO 建议保持关闭。

### Status

Round 1 已完成实验闭环；外部 MCP reviewer 不可用，已用多代理 reviewer 替代。
