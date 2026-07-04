# Linux HugePage 优化 RaBitQ Resident 查询方案

## 背景

当前在线查询会在 `OverlapScheduler::Search()` 中确保 `cluster.clu` 已经 resident preload。`ClusterStoreReader::PreloadAllClusters()` 有两种 resident 模式：

- `compact_batched`，默认路径：分批读取 `cluster.clu`，为每个 cluster 复制一份 `code_storage`，并可构造 resident parallel Stage2 view。
- `full_file`，通过 `VDB_RESIDENT_PRELOAD_MODE=full_file` 开启：一次性读取整个 `cluster.clu` 到 `resident_file_buffer_`，各个 `ParsedCluster` 直接指向这块连续 buffer。

Linux 当前 THP 配置为 `madvise` 模式，因此匿名堆内存默认不会自动使用 HugePage。若要验证 HugePage 对内存中 RaBitQ scan 的收益，需要显式调用 `madvise(MADV_HUGEPAGE)`。

## 已实现的最小实验开关

新增环境变量：

```bash
VDB_RESIDENT_HUGEPAGE=1
```

开启后，在 resident preload 阶段对以下对象调用 `madvise(MADV_HUGEPAGE)`：

- `resident_file_buffer_`：full-file 模式下的整块 `cluster.clu` buffer。
- `ResidentClusterView::code_storage`：compact 模式下每个 cluster 的 code region，只有单个 vector 大于等于 2MB 时才触发。
- `exrabitq_parallel_abs_blocks_storage` / `exrabitq_parallel_sign_words_storage`：resident Stage2 parallel view buffer，只有大于等于 2MB 时触发。

默认不开启，不影响已有结果。

## 预期效果

HugePage 主要可能降低 dTLB miss，对以下场景更可能有效：

- `full_file` resident preload：ESCI 的 `cluster.clu` 约 790MB，是连续匿名内存，适合 THP。
- 大 `cluster.clu`、高 `nprobe`、内存 scan 占比较高的查询。

对以下场景预期收益较弱：

- 默认 `compact_batched`：每个 cluster 的 `code_storage` 通常远小于 2MB，THP 很难覆盖。
- 当前 `vector_bitmajor_tiles` 直接格式：resident parallel Stage2 view 基本为 0，HugePage 主要只影响 Stage1/FastScan 与 Stage2 原始 code region 的访存。
- I/O 或 coarse routing 占主导时，HugePage 不会直接优化这些部分。

## 实验设计

先只在 `amazon_esci`、`ex_bits=3`、`vector_bitmajor_tiles` 上验证：

1. 默认 compact preload：
   - `VDB_RESIDENT_PRELOAD_MODE` 不设置
   - `VDB_RESIDENT_HUGEPAGE=0/1`
2. full-file preload：
   - `VDB_RESIDENT_PRELOAD_MODE=full_file`
   - `VDB_RESIDENT_HUGEPAGE=0/1`

查询参数保持和当前优化实验一致：

```bash
topk=100
nprobe=256
two_level_coarse_routing=1
two_level_coarse_budget_factor=12
non_safeout_candidate_budget=400
queries=1000
fine_grained_timing=0
```

记录指标：

- `avg_query_time_ms`, `recall_at_k`
- `avg_probe_stage1_ms`, `avg_probe_stage2_ms`, `avg_probe_submit_ms`
- resident memory fields：`resident_file_buffer_bytes`, `resident_code_storage_bytes`, `resident_cluster_mem_bytes`
- OS 侧：`AnonHugePages` 或 `smaps_rollup` 中的 huge page 相关字段

如果环境允许，再用：

```bash
perf stat -e dTLB-loads,dTLB-load-misses,cycles,instructions,LLC-load-misses
```

比较 HugePage 开关前后的 TLB miss 变化。

## 判定标准

- 若 full-file + HugePage 能稳定降低 `avg_query_time_ms` 或 `probe_stage1/probe_stage2` 超过 3%，并且 `smaps_rollup` 显示产生 `AnonHugePages`，则保留为可选系统优化。
- 若 `AnonHugePages` 始终为 0，或延迟变化小于噪声，说明当前分配方式/内核策略无法有效使用 THP，不应作为主贡献。
- 若 full-file 模式本身显著慢于 compact 模式，则 HugePage 即使有效，也只作为诊断和特定部署优化，不替代默认 resident preload。

## 风险

- `madvise(MADV_HUGEPAGE)` 是提示，不保证内核一定分配 huge page。
- full-file 模式会保留整块 `cluster.clu`，内存占用高于 compact 模式。
- 显式 `MAP_HUGETLB` 暂不建议使用，因为需要系统预留 huge pages，且 2MB 粒度会放大小 cluster 的内存浪费。
