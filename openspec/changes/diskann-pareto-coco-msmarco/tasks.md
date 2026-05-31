## 1. Baseline 盘点与正确性校验

- [x] 1.1 核对当前 C++ DiskANN 可执行路径，并记录搜索与建索引实际使用的二进制文件。
- [x] 1.2 核对 COCO100k 的 base vectors、query vectors、ground-truth top-k 文件、现有 DiskANN 索引目录，以及 FlatStor payload 目录。
- [x] 1.3 核对 MSMARCO formal baseline 的 base vectors、query vectors、ground-truth top-k 文件、清洗后的 payload 资产，以及 FlatStor payload 目录。
- [x] 1.4 增加或更新共享的修正 recall helper，按平均 top-k overlap 计算 recall，并忽略无效 sentinel ID。
- [x] 1.5 增加结果有效性检查，包括重复 ID、top-k 行不完整、query 数量不匹配、payload 缺失、CLI 解析失败和超时。

## 2. DiskANN C++ Runner 与 FlatStor 计时

- [x] 2.1 扩展 C++ DiskANN runner，使 dataset 配置同时覆盖 `coco_100k` 和 `msmarco_passage`。
- [x] 2.2 让 runner 负责写出 DiskANN query `.bin` 文件、调用 C++ `search_disk_index`、解析结果 ID/距离文件，并按需保存 top-k ID 工件。
- [x] 2.3 增加索引 manifest 的读取或生成功能，记录 index identity、构建参数、数据路径、ground-truth 路径和 companion 文件状态。
- [x] 2.4 为 DiskANN top-k ID 增加 FlatStor payload 读取计时，并将 payload latency 与 search latency 分开导出。
- [x] 2.5 导出原始 sweep 行，字段包含 dataset、index identity、搜索参数、recall、search latency、payload latency、combined latency、tail latency、duplicate rate、status 和 invalid reason。

## 3. COCO100k 首轮 Pareto Sweep

- [ ] 3.1 在运行 C++ 搜索前，校验或重建现有 COCO100k DiskANN 索引所需的 companion 文件。
- [ ] 3.2 使用现有 `R=64,L_build=100` 索引，对 `L_search` 和 `beam_width` 进行 COCO100k 粗扫。
- [ ] 3.3 围绕 `0.85`、`0.90`、`0.95`、`0.98` 和 `0.995` 的 recall crossing 进行定向细化。
- [ ] 3.4 如果低 recall 区间仍未覆盖，在构建新 COCO 索引之前先跑现有更弱的 `R=32,L_build=50` 索引。
- [ ] 3.5 如果两个现有 COCO100k 索引仍无法覆盖所需区间，则额外构建一个更弱的 COCO100k 索引，只补跑缺失的低 recall 区域。
- [ ] 3.6 写出 COCO100k 的原始 sweep CSV、有效 Pareto frontier CSV、目标 recall 汇总，以及选定点对应的 top-k ID 工件。

## 4. MSMARCO 索引构建

- [ ] 4.1 增加或更新 MSMARCO 到 DiskANN 的导出步骤，生成行对齐稳定的 base/query `.bin` 文件。
- [ ] 4.2 构建第一个 MSMARCO disk index，先采用中等配置，例如 `R=32,L_build=50`，并记录 disk-PQ byte 预算。
- [ ] 4.3 在 MSMARCO 索引目录旁写出 manifest，记录源数据路径、metric、维度、构建命令、DiskANN binary、构建参数和时间戳。
- [ ] 4.4 校验或生成 MSMARCO 索引在 C++ DiskANN loader 下所需的 companion 文件。
- [ ] 4.5 先跑一个小规模 MSMARCO smoke search，确认修正 recall、duplicate rate、结果解析和 FlatStor payload 读取都正常，再开始全量 sweep。

## 5. MSMARCO Pareto Sweep 与自适应扩索引

- [ ] 5.1 在第一个 MSMARCO 索引上，对 `L_search` 和 `beam_width` 进行 MSMARCO 粗扫。
- [ ] 5.2 围绕第一个 MSMARCO 索引实际达到的目标 recall 阈值做定向细化。
- [ ] 5.3 如果高 recall 目标无法达到，则构建更强的 MSMARCO 索引，并只补跑缺失的高 recall 区域。
- [ ] 5.4 如果所有 MSMARCO 点都过高 recall 或过慢，不利于对比，则构建更弱的 MSMARCO 索引，并补跑缺失的低 recall 区域。
- [ ] 5.5 写出 MSMARCO 的原始 sweep CSV、有效 Pareto frontier CSV、目标 recall 汇总，以及选定点对应的 top-k ID 工件。

## 6. 聚合与报告

- [ ] 6.1 将 COCO100k 与 MSMARCO 的 DiskANN 结果聚合成一个可直接对比的统一结果文件，并保持列名稳定。
- [ ] 6.2 按数据集生成 Pareto 汇总，在每个目标 recall 阈值上选择满足条件的最快有效点。
- [ ] 6.3 将 DiskANN 的 combined latency 字段与对应 VecFetch 计时口径逐项对齐，并明确记录任何 scope mismatch。
- [ ] 6.4 写一份简短实验说明，记录执行命令、复用或新建的索引、无效点、recall 覆盖情况，以及剩余待补跑内容。
- [ ] 6.5 运行脚本和解析器的验证命令，并记录任何未能执行的测试。
