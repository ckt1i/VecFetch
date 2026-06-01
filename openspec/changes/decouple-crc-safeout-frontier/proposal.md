## Why

当前查询路径把动态 SafeOut frontier 的维护错误绑定到 CRC early-stop：关闭 `early_stop` 后若仍想保留 SafeOut 剪枝，就必须继续启用 `crc_params`。这会让 CRC 的 probe 早停、estimate heap、dynamic SafeOut frontier 三个概念混在一起，也使 `--crc 0` 无法评估“无 CRC 早停但有 SafeOut 剪枝”的工作点。

同时，现有 frontier 使用按 `d_hat` 保留的 top-k，再取这些候选的上界最大值；后续应改成更直接的 upper-bound top-k frontier，使 SafeOut 判定和候选级误差界 `e_i` 对齐。

## What Changes

- 解耦 CRC early-stop 与 dynamic SafeOut frontier：
  - CRC early-stop 只负责 `CrcStopper::ShouldStop()` 和 probe-loop break。
  - dynamic SafeOut 独立维护 estimate frontier，即使 `early_stop=false` 或 CRC early-stop 关闭，也可以启用。
- 将 SafeOut frontier 改为基于 candidate upper bound 的 top-k：
  - 对保留下来的候选计算 `U_i = d_hat_i + e_i`。
  - 维护当前 query 已见候选中的 top-k smallest `U_i`。
  - 当该 heap 满足 `top_k` 时，设置 `F = max_{j in topk_by_U}(U_j)`，即 `kth_smallest(U)`。
  - SafeOut 使用 `L_i = d_hat_i - e_i > F`。
- 保留 cluster 级 frontier 快照：
  - 进入一个 cluster 时读取一次 `F`。
  - 当前 cluster 内 Stage1/Stage2 使用同一个 `F`。
  - 当前 cluster 产生的候选只影响后续 cluster。
- 更新 benchmark 配置和输出语义：
  - 支持“CRC early-stop 关闭，但 dynamic SafeOut 开启”的配置。
  - 输出中区分 CRC 早停统计和 SafeOut frontier 统计。
- 不改变 RaBitQ/FastScan 距离估计 kernel，不改变 final exact rerank 语义。

## Capabilities

### New Capabilities

无。

### Modified Capabilities

- `dynamic-safeout`: 将 SafeOut frontier 从 CRC 绑定状态中解耦，并改为 top-k upper-bound frontier。
- `crc-early-stop`: 明确 CRC early-stop 只控制 probe-loop 是否提前停止，不再作为 dynamic SafeOut frontier 的启用条件。

## Impact

- 主要影响：
  - `include/vdb/query/search_context.h`
  - `src/query/overlap_scheduler.cpp`
  - `include/vdb/index/cluster_prober.h`
  - `src/index/cluster_prober.cpp`
  - `include/vdb/index/conann.h`
  - Stage1/Stage2 SafeOut 相关 SIMD 分类路径
- Benchmark 影响：
  - `benchmarks/bench_vector_search.cpp`
  - `benchmarks/bench_e2e.cpp`
  - 相关 replay/diagnostic 工具如继续报告 SafeOut frontier，需要同步字段名和公式说明。
- 风险：
  - 按 `U=d_hat+e` 维护 top-k 会比按 `d_hat` 多一点 heap 维护成本，但 `top_k` 很小，预计开销可控。
  - 新 frontier 更保守、更语义正确，但 SafeOut 数量可能相对旧实现下降，需要通过 COCO100k 验证量化。
