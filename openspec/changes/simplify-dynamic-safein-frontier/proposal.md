## Why

最近几轮 Dynamic SafeIn 实验加入了多种探索性模式和诊断字段，但目前认可的设计已经收敛为更窄的方案：只保留基于查询在线 lower-bound top-k frontier 的 SafeIn prefetch，并通过 deferred candidate buffer 延迟早期判断。删除未采用的分支并将 `frontier_blend` 更名为 `frontier`，可以降低查询路径复杂度，也避免 benchmark 参数继续暗示存在不再支持的调参空间。

## What Changes

- **BREAKING**：将 `--dynamic-safein frontier_blend` 更名为 `--dynamic-safein frontier`。
- **BREAKING**：删除不再支持的 Dynamic SafeIn 模式：`frontier_cap`、`frontier_delay`、`frontier_stable`、`frontier_scale`。
- **BREAKING**：删除基于 lambda/scale 的 SafeIn 控制项，包括 `--dynamic-safein-scale` 和 `--dynamic-safein-scale-cap-static`。
- 删除不属于最终设计的质量 gate 和 payload-only 实验控制项，包括 gap tolerance 和 payload-only gating。
- 保留 `static/off` baseline，以及已接受的 `frontier` 模式；该模式下 query-adaptive SafeIn 阈值为当前 lower-bound top-k frontier。
- 保留 deferred candidate buffering 和 reclassification，用于当前 `defer_initial_clusters` / `defer_until_ready` 机制。
- 删除仅服务于已废弃模式或 gate 的 benchmark 输出字段，同时保留用于解释剩余 frontier 路径的 prefetch/read accounting。
- 更新测试和实验文档，将命令和语义迁移到 `frontier` 与固定 `T_q = F_lower`。

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `query-pipeline`：`OverlapScheduler` 的 Dynamic SafeIn 行为收敛为 `static/off` 与唯一的 `frontier` 模式，并采用固定 lower-frontier 阈值语义。
- `benchmark-infra`：简化 Dynamic SafeIn benchmark 的 CLI、config output、JSON output、CSV output 和日志字段，只保留仍支持的模式与字段。

## Impact

- 受影响代码：`include/vdb/query/search_context.h`、`include/vdb/query/overlap_scheduler.h`、`src/query/overlap_scheduler.cpp`、`benchmarks/bench_vector_search.cpp`、`benchmarks/bench_e2e.cpp`、`tests/query/overlap_scheduler_test.cpp`。
- 受影响脚本/文档：提到废弃模式或 `frontier_blend` 的 Dynamic SafeIn 实验脚本与 review logs。
- API/CLI 兼容性：使用废弃模式或 scale/gap/payload-only flags 的旧实验命令会失败，需要迁移；已接受命令应改为 `--dynamic-safein frontier`。
- Runtime 行为：Static SafeIn 保持可用；frontier 模式不再暴露 lambda 插值，ready 后使用 `T_q = F_lower`。
