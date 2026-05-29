## Why

接下来要做 COCO100k 和 MSMARCO 上的正式对比，DiskANN 是必须纳入的外部 baseline。但此前的 Python 绑定路径会把 recall 算高，而且会返回重复的 top-k ID，因此现有结果不能直接用于 Pareto 对比。我们需要一条可复现的 C++ DiskANN 流程，统一输出修正后的 `recall@10`，并按照与 VecFetch 一致的口径计时：`search(DiskANN) + read_payload(FlatStor)`。

## What Changes

- 为 COCO100k 和 MSMARCO 增加一套 DiskANN Pareto 评测流程。
- 将 C++ DiskANN CLI 路径作为正式测量结果的唯一权威搜索路径。
- 将 recall 统一为基于预计算 ground truth 的 top-k overlap 平均值，而不是“单 query 只要命中任意一个就算对”的旧口径。
- COCO100k 先使用现有已验证的 DiskANN 索引族，优先扫描搜索期参数，再决定是否需要额外构建 COCO 索引。
- 为 MSMARCO 增加 DiskANN 索引构建支持，并记录 C++ DiskANN loader 所需的 companion 文件与索引参数清单。
- 在首个 MSMARCO 索引构建完成后再跑 MSMARCO，并根据 recall 覆盖情况决定是否继续构建更强或更弱的后续索引。
- 输出可直接用于 Pareto 分析的结果，分别记录 DiskANN 搜索延迟、FlatStor payload 读取延迟，以及合并后的端到端延迟。
- 对重复 ID、ground truth 未对齐、payload 缺失、top-k 不完整等无效点进行明确标记或剔除。

## Capabilities

### New Capabilities

- `diskann-pareto-evaluation`: 面向 COCO100k 和 MSMARCO 的可复现 DiskANN C++ CLI 建索引、搜索、FlatStor payload 计时、修正 recall 校验与 Pareto 聚合能力。

### Modified Capabilities

无。

## Impact

- 影响 `baselines/vector_search/` 下的实验脚本，尤其是 C++ DiskANN runner，以及可能新增的 MSMARCO 索引构建辅助脚本。
- 影响 `baselines/data/` 或 `/home/zcq/VDB/baselines/data/` 下的生成数据，包括 MSMARCO DiskANN 索引和 DiskANN companion 文件。
- 影响 `baselines/results/` 下的结果工件，包括原始 sweep CSV、目标 operating point 汇总，以及后续的 Pareto 图表或表格输出。
- 依赖 `/home/zcq/VDB/third_party/DiskANN/build/apps/` 下现有的 DiskANN 构建产物、正式版 COCO100k/MSMARCO ground truth 文件，以及现有 FlatStor payload 后端。
- 不修改 VecFetch 的查询语义、payload 存储格式，也不改变现有 benchmark 基础设施的契约。
