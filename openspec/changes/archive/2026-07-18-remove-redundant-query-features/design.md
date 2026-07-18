## Context

当前 resident 查询主路径已经完成多轮优化和对比，技术冻结方案倾向于保留稳定收益路径，并停止维护负收益或收益不稳定的实验分支。现有代码中仍存在多组历史优化开关：Stage2 progressive pruning、Stage1 block skip envelope、address sorting、budgeted early submit，以及若干与这些开关绑定的 CLI、JSON 统计和测试用例。

这些功能的共同问题是：它们扩大了查询配置空间和回归测试矩阵，但没有进入最终冻结方案。继续保留这些上层入口会使后续论文实验和代码维护更容易误用非冻结路径。

## Goals / Non-Goals

**Goals:**

- 删除负收益实验功能的用户可见入口、配置字段、主路径分支和正式统计字段。
- 保留冻结主路径：
  - fixed active ex_bits 查询。
  - selective resident preload。
  - compact batched preload。
  - two-level coarse routing，默认 `budget_factor=16`。
  - SafeIn/SafeOut 和 prefetch pipeline。
  - `tile_lane_bitmajor` 方法格式和 `vector_bitplanes` official-like baseline/兼容格式。
- 对 SIMD 采取保守策略：不删除底层 helper/kernel，只删除已放弃功能对其的调用和 CLI。
- 实现后用同一已有索引和同一 warm query 参数进行删除前后对比，确认 recall 和速度没有超过误差范围的损失。

**Non-Goals:**

- 不重写 RabitQ stage2 存储格式。
- 不删除仍可能用于 official-like baseline、旧索引读取或固定路径的 SIMD kernel。
- 不删除 two-level coarse routing、selective preload、compact batched preload、SafeIn/SafeOut pipeline。
- 不以本 change 重新解释论文贡献或新增实验结论。
- 不把旧实验脚本的所有输出字段保持为兼容承诺；废弃字段可以移除。

## Decisions

### Decision 1: 先删除上层功能入口，不激进删除 SIMD kernel

实现将移除 progressive pruning 的 CLI、配置、统计、`ClusterProber` 分支和 scheduler 聚合，但保留相关 SIMD helper/kernel 源码。这样可以降低误删 shared helper 的风险，也保留后续单独清理 SIMD 文件的空间。

Alternative considered: 同时删除 progressive-only SIMD helper。  
Rejected because SIMD 文件内存在多个 layout 和测试路径共享实现细节，激进删除会增加回归风险，不符合本轮“保守 SIMD”的约束。

### Decision 2: Stage1 block skip envelope 整体删除

Stage1 envelope 需要额外 resident summary、parsed cluster 字段、probe 判断和统计输出，但历史结果显示 skip 率极低且明显变慢。本 change 将删除其配置、preload summary、parsed view、prober 判断、scheduler 汇总、benchmark 输出和对应测试。

Alternative considered: 仅隐藏 CLI，保留内部代码。  
Rejected because Stage1 envelope 会继续污染 resident preload 内存布局和统计路径，且没有保留价值。

### Decision 3: address sorting 和 budgeted early submit 从正式查询路径移除

这两个功能属于 pipeline 调度实验分支，不在技术冻结方案中。实现将删除 CLI/config/stats 以及 scheduler 中的排序和预算提前提交路径。主路径继续使用冻结的 batch submit / fixed flush / SafeIn-SafeOut 逻辑。

Alternative considered: 保留为 hidden flag。  
Rejected because隐藏后仍会扩大测试矩阵，并可能被后续脚本误打开。

### Decision 4: layout 只收敛用户可见面，不在本轮删除兼容 reader

本轮不删除 `vector_bitplanes` 和 `tile_lane_bitmajor`，也不主动破坏旧索引读取。若代码中存在未进入冻结方案的 layout 变体，本轮优先从 benchmark 参数、默认选择和实验脚本入口中移除；底层 reader/writer 分支只有在确认没有兼容需求时才删除。

Alternative considered: 删除所有非冻结 layout enum 和 reader。  
Rejected because旧索引和 official-like baseline 对照仍可能依赖兼容格式，删除会把清理 change 变成数据迁移 change。

### Decision 5: 验证以“同索引、同参数、warm query”为准

删除不应改变算法语义。验证必须复用已有冻结索引，并固定 dataset、queries、topk、nprobe、two-level coarse routing、resident preload、active/resident ex_bits。通过标准：

- `recall@10` 绝对差不超过 `0.002`，预期为完全一致。
- `avg_ms` 不变慢超过 `3%`，或 QPS 不下降超过 `3%`。
- `avg_total_probed`、rerank/candidate 计数不应出现结构性变化；若变化超过 `1%`，必须解释或回退。
- 若运行噪声明显，重复运行并报告均值。

## Risks / Trade-offs

- [Risk] 废弃 CLI 被旧脚本使用，脚本会失败。  
  Mitigation: 在 tasks 中加入脚本搜索和报错信息清理，必要时在文档列出迁移方式。

- [Risk] 删除 stats 字段导致结果汇总脚本读取失败。  
  Mitigation: 同步更新 benchmark JSON 写出和已有聚合脚本，保留正式字段。

- [Risk] Stage1 envelope 字段删除影响 resident preload struct 初始化。  
  Mitigation: 按数据流从 storage view 到 parsed cluster 再到 prober 顺序删除，并运行相关 unit tests。

- [Risk] budgeted early submit 逻辑与 scheduler 状态机交织，删除可能影响 flush/drain。  
  Mitigation: 分小步删除，保留现有 fixed submit/flush 流程，重点测试 resident vector-only submit、tail flush 和 final drain。

- [Risk] 删除 layout 入口时误伤 baseline。  
  Mitigation: 本轮不删除 conservative SIMD 和 baseline layout；只收敛实验性入口。

## Migration Plan

1. 记录删除前最佳结果的位置和参数，作为验证基线。
2. 删除 Stage1 envelope 功能面。
3. 删除 progressive pruning 功能面，保留 SIMD helper。
4. 删除 address sorting 和 budgeted early submit 功能面。
5. 清理 benchmark CLI/JSON 字段和相关 tests。
6. 构建并运行单元测试。
7. 复用已有索引运行 warm query 回归，并输出删除前后对比表到 `/home/zcq/VDB/test`。
8. 如果 recall 或速度超过误差范围，回退最近一类删除并定位具体分支。

## Open Questions

- 是否需要保留 deprecated flag 的显式错误提示一个版本，还是直接删除参数解析。
- 哪些历史聚合脚本仍消费 progressive 或 envelope 统计字段，需要在 apply 阶段通过脚本搜索确认。
