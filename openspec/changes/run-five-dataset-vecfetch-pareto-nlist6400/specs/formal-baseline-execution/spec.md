## ADDED Requirements

### Requirement: 正式执行必须用复用索引运行五数据集 Full VecFetch Pareto
正式实验工作流必须只使用已接受的预先存在 VecFetch 索引，为 `coco_100k`、`msmarco_passage`、`amazon_esci`、`imagenet1k` 和 `voxceleb2_ecapa_150k` 执行 Full VecFetch Pareto sweep。

#### Scenario: 调度前验证 accepted VecFetch 索引
- **WHEN** 五数据集 VecFetch Pareto 工作流启动
- **THEN** runner 必须在调度任何 benchmark 命令前验证以下 dataset-to-index map：
  - `coco_100k`: `/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90`
  - `msmarco_passage`: `/home/zcq/VDB/test/data/MSMARCO/fht_kac_rotator`
  - `amazon_esci`: `/home/zcq/VDB/test/data/amazon_esci/vecfetch_nlist8192_np64_bits4_raw_payload/index`
  - `imagenet1k`: `/home/zcq/VDB/test/data/imagenet1k_split_v1/vecfetch_nlist6400_np64_bits4_raw_payload/index`
  - `voxceleb2_ecapa_150k`: `/home/zcq/VDB/test/data/voxceleb2_ecapa_150k/vecfetch_nlist2048_np64_bits4_audio_payload_split_v1/index`
- **AND** 如果任何必需索引缺失、为空或 `nlist` metadata 不匹配，runner 必须在 benchmark 执行前失败。

#### Scenario: VecFetch 查询运行不得构建索引
- **WHEN** 生成正式 Full VecFetch Pareto 命令
- **THEN** 命令必须显式传入指向该数据集 accepted index path 的 `--index-dir`
- **AND** 命令不得依赖隐式索引构建路径
- **AND** 命令不得删除、覆盖或重建 accepted index directory。

### Requirement: Full VecFetch Pareto sweep 必须使用固定方法控制项和数据集专属 nprobe 网格
正式 Full VecFetch Pareto sweep 必须只改变 `topk` 和 `nprobe`，并固定已接受的方法控制项。

#### Scenario: 每个 Full VecFetch 行固定方法控制项
- **WHEN** 运行一个正式 Full VecFetch 行
- **THEN** 它必须使用 `topk` in `{10,100}`
- **AND** 必须启用 Dynamic SafeOut
- **AND** 必须使用 Dynamic SafeIn mode `frontier`
- **AND** 必须使用 `dynamic_safein_stable_probes=1`
- **AND** 必须使用 `dynamic_safein_rel_tol=0.005`
- **AND** 必须使用 `dynamic_safein_defer_initial_clusters=4`
- **AND** 必须使用 `dynamic_safein_defer_until_ready=1`
- **AND** 必须使用 `clu_read_mode=full_preload`
- **AND** 必须使用 resident clusters
- **AND** 必须使用 `prefetch_depth=16`
- **AND** 必须使用 `io_queue_depth=64`
- **AND** 必须使用 `fixed_vec_buffer_count=1024`
- **AND** 必须使用 `bits=4`。

#### Scenario: 使用数据集专属 Full VecFetch nprobe 网格
- **WHEN** 生成 Full VecFetch Pareto 命令
- **THEN** `coco_100k` 必须使用 `nprobe={16,32,64,128,256,512}`
- **AND** `msmarco_passage` 必须使用 `nprobe={16,32,64,128,256,512,1024}`
- **AND** `amazon_esci` 必须使用 `nprobe={16,32,64,128,256,512}`
- **AND** `imagenet1k` 必须使用 `nprobe={16,32,64,128,256,512}`
- **AND** `voxceleb2_ecapa_150k` 必须使用 `nprobe={8,16,32,64,128,256}`。

### Requirement: ImageNet1K IVF baseline 必须以 nlist 6400 和六点 nprobe 网格重跑
正式 baseline 工作流必须在 `nlist=6400` 下重跑 ImageNet1K IVF+PQ 和 IVF+RQ baseline，使 baseline search surface 与更新后的 ImageNet1K VecFetch 索引一致。

#### Scenario: ImageNet1K IVF nlist 6400 重跑矩阵固定
- **WHEN** 调度 ImageNet1K IVF baseline 重跑
- **THEN** 必须运行 `IVF+PQ` 和 `IVF+RQ`
- **AND** 必须运行 `flatstor` 和 `lance` 两个 payload backends
- **AND** 必须运行 `topk={10,100}`
- **AND** 必须使用 `nlist=6400`
- **AND** 只能使用 `nprobe={16,32,64,128,256,512}`
- **AND** 不得调度 `nprobe=96`、`192`、`384`、`768` 或 `1024`。

#### Scenario: IVF 重跑使用固定 baseline 控制项
- **WHEN** 运行 ImageNet1K IVF+PQ `nlist=6400` 行
- **THEN** 必须使用 `m=64`、`nbits=8` 和 `candidate_budget=topk*20`
- **AND** 同一 ANN 参数必须在 FlatStor 与 Lance 上 replay。

#### Scenario: IVF+RQ vector output 在 payload backends 间复用
- **WHEN** 运行 ImageNet1K IVF+RQ `nlist=6400` 行
- **THEN** 给定 `topk`、`nprobe` 和 candidate budget 的 vector-search output 必须可在 FlatStor 与 Lance coupled E2E 行之间复用
- **AND** baseline 必须使用 `total_bits=4` 和 `candidate_budget=topk*20`。

### Requirement: 正式执行必须在昂贵运行前执行 readiness 与 safety gates
工作流必须在 full sweep 前检查 dataset readiness、CPU 空闲、磁盘容量和命令安全性。

#### Scenario: full execution 前 preflight 通过
- **WHEN** 即将启动完整实验 batch
- **THEN** runner 必须确认五个数据集都有 `gt_top100.npy`
- **AND** 必须确认剩余磁盘空间足够写入输出
- **AND** 必须确认没有冲突的长时间 benchmark 进程
- **AND** 必须在 change outputs 中记录 preflight 结果。

#### Scenario: smoke runs gate full sweeps
- **WHEN** accepted indexes 和 adapters 通过 readiness checks
- **THEN** runner 必须在 `nprobe=64` 属于该数据集网格时，为 `topk=10` 和 `topk=100` 执行 smoke point
- **AND** 只有 smoke point 从 `gt_top100.npy` 报告可用 recall 后，full sweep 才能继续。
