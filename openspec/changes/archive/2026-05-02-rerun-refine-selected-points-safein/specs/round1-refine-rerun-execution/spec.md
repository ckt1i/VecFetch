## ADDED Requirements

### Requirement: Round 1 补实验执行范围锁定为已选矩阵
Round 1 补实验执行合同仅覆盖论文评审相关的补跑点，不扩大到其它无关实验。

#### Scenario: 补实验范围被调度
- **WHEN** 启动 Round 1 补实验
- **THEN** 数据集必须为 `coco_100k` 与 `msmarco_passage`
- **AND** 必执行的块为 COCO matched-quality 选点重跑、COCO SafeIn 重复、MS MARCO SafeIn 重复、RaBitQ baseline 清理
- **AND** top-k 压力测试、图索引、 新数据集、新后端、GPU/并发测试不在本次范围内

#### Scenario: 查询与召回协议固定
- **WHEN** 执行 Round 1 的正式测量
- **THEN** 使用 `queries=1000`
- **AND** 使用 `topk=10`
- **AND** 采用 strict recall 并使用批准的 GT
- **AND** 不得使用 `skip_gt=1`

### Requirement: Round 1 重跑复用 canonical artifact
Round 1 重跑必须复用 `thesis-minimal-main-sweep` 中已建立的 canonical artifact，除非来源溯源校验失败。

#### Scenario: COCO 选点与 SafeIn 复用 COCO canonical artifact
- **WHEN** 调度 COCO 的选点或 SafeIn 重跑
- **THEN** 使用现有 `coco_100k` canonical artifact（`nlist=2048`、`bits=4`、`faiss_kmeans`、single-assignment）
- **AND** VecFetch 与 `IVF+RaBitQ+FlatStor` 使用同一 centroid / assignment 溯源

#### Scenario: MS MARCO SafeIn 复用 MS MARCO canonical artifact
- **WHEN** 调度 MS MARCO SafeIn
- **THEN** 使用现有 `msmarco_passage` canonical artifact（`nlist=16384`、`bits=4`、`hierarchical_superkmeans`、single-assignment）
- **AND** 复用语义与 thesis 主线一致

### Requirement: COCO 选点重跑显式建模 `candidate_budget`
COCO matched-quality 重跑必须把 `IVF+RaBitQ+FlatStor` 的 `candidate_budget` 视为必要参数，而非只比对 `nprobe`。

#### Scenario: COCO 选点网格被执行
- **WHEN** 执行 COCO 的选点网格
- **THEN** VecFetch 在 `nprobe=64`、`128`、`256` 下测量
- **AND** `IVF+RaBitQ+FlatStor` 在 `nprobe=64`、`128`、`256` 下测量
- **AND** 每条 RaBitQ 行记录 `candidate_budget`
- **AND** `candidate_budget` 必须包括 `100`、`256`、`512`（runner 支持范围内）

#### Scenario: 扩展 `candidate_budget` 被数据合约阻塞
- **WHEN** `candidate_budget=256` 或 `512` 的 COCO 预算扫因输入/GT 合约不匹配而作废
- **THEN** 必须在 tasks 与汇总中记录为 blocked/out-of-scope
- **AND** 不得将作废行写入论文可见对比
- **AND** 若已有几乎同 recall 的 `candidate_budget=100` 对齐点可回答 reviewer concern，则该阻塞不阻止 Round 1 论文更新
- **AND** 后续若仍需预算扫，必须新开 change 先恢复 COCO 数据/GT 合约

#### Scenario: 可选 COCO `nprobe` 点有条件触发
- **WHEN** 必须网格无法给出可解释的统一阈值或窄带匹配
- **THEN** 可调度补测 `nprobe=96`、`160`、`192`
- **AND** 补测点标记为补充而非论文主表必需点

#### Scenario: VecFetch 主线参数固定
- **WHEN** 在 Round 1 的选点或 SafeIn 中测量 VecFetch
- **THEN** 使用 `crc=1`
- **AND** 使用 `early-stop=0`
- **AND** 使用 `bits=4`
- **AND** 记录 SafeOut、Uncertain、SafeIn 与 reranked-candidate 数

### Requirement: SafeIn 重复执行固定为一次预热和三次测量
SafeIn 重复实验采用你要求的 reduced 版本：每组先一次 warmup，随后每个变体三次正式测量。

#### Scenario: COCO SafeIn 重复块执行
- **WHEN** 调度 COCO SafeIn 重复
- **THEN** 数据集为 `coco_100k`
- **AND** 运行点为 `topk=10`、`nprobe=64`
- **AND** 变体为 Full 与 SafeIn-off
- **AND** 先执行 1 次 warmup
- **AND** 每个变体有 3 次测量重复

#### Scenario: MS MARCO SafeIn 重复块执行
- **WHEN** 调度 MS MARCO SafeIn 重复
- **THEN** 数据集为 `msmarco_passage`
- **AND** 运行点为 `topk=10`、`nprobe=128`
- **AND** 变体为 Full 与 SafeIn-off
- **AND** 先执行 1 次 warmup
- **AND** 每个变体有 3 次测量重复

#### Scenario: SafeIn warmup 不计入统计
- **WHEN** 计算 SafeIn 重复统计
- **THEN** warmup 保留审计性
- **AND** warmup 不参与 mean、median、std、min、best 或论文面向的延迟统计

### Requirement: 最终 RaBitQ baseline 清理只覆盖论文可见点
最终的 RaBitQ baseline 清理仅重跑论文可见或 matched-quality 比较所需点。

#### Scenario: 清理重复复现最终选定行
- **WHEN** 已完成论文可见行选择
- **THEN** 每个选定行在同一 canonical artifact、`nprobe`、`topk`、`candidate_budget` 下重跑
- **AND** 每个最终点包含 1 次 warmup
- **AND** 每个最终点包含 3 次测量

#### Scenario: 清理不重做全集
- **WHEN** 启动 baseline 清理
- **THEN** 不重跑无关的 PQ、Lance、附录、或非选定 RaBitQ 点
- **AND** 不可执行或 unsupported 的清理行记录明确原因

#### Scenario: MS MARCO cleanup 未进入本轮
- **WHEN** MS MARCO RaBitQ cleanup 未在当前 Round 1 步骤执行
- **THEN** 必须记录为 blocked/out-of-scope
- **AND** 不得声称已完成 MS MARCO baseline cleanup
- **AND** 该阻塞不影响 COCO matched-quality 与 SafeIn 重复实验的论文更新决策
