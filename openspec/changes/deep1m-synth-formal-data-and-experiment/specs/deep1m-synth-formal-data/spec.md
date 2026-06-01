## ADDED Requirements

### Requirement: Deep1M_synth formal dataset contract

系统 SHALL 将 `deep1m_synth` 物化到 formal-baseline 根目录下，并补齐 Python baseline runner 与 C++ `bench_e2e` adapter 所需的全部文件。

#### Scenario: Formal vectors are present

- **WHEN** 运行 `Deep1M_synth` 数据验证阶段
- **THEN** `/home/zcq/VDB/data/formal_baselines/deep1m_synth/embeddings/base_embeddings.npy` MUST 存在，shape 为 `(1000000, 96)`，dtype 为 `float32`
- **AND** `/home/zcq/VDB/data/formal_baselines/deep1m_synth/embeddings/query_embeddings.npy` MUST 存在，shape 为 `(10000, 96)`，dtype 为 `float32`
- **AND** formal dataset manifest MUST 记录 `metric=l2`、`normalization=l2_unit_norm` 和 `dataset_id=deep1m_synth`
- **AND** manifest MUST 明确注明：由于公开 Deep1M 向量已经做 L2 归一化，本实验中的欧氏距离（L2）排序与余弦相似度排序等价

#### Scenario: Bench adapter is present

- **WHEN** 运行 `Deep1M_synth` canonical build 阶段
- **THEN** bench adapter 目录 MUST 提供 `image_embeddings.npy`、`image_ids.npy`、`query_embeddings.npy`、`query_ids.npy` 和 `metadata.jsonl`
- **AND** `image_ids.npy` MUST 恰好包含 `0..999999`
- **AND** `query_ids.npy` MUST 与持久化后的 formal split 对应

### Requirement: Deep1M_synth split and ground truth alignment

系统 SHALL 为 `Deep1M_synth` 定义一个固定 split，并由该 split 派生全部 formal GT 文件。

#### Scenario: Split is persisted

- **WHEN** 数据准备阶段创建 `splits/split_v1.json`
- **THEN** 其中 MUST 记录 `query_count=1000`
- **AND** MUST 记录这 1000 个被选中的 query index
- **AND** 每个被选中的 index MUST 落在 `[0, 10000)` 范围内

#### Scenario: GT rows match split

- **WHEN** 生成 formal GT 文件
- **THEN** `gt_top10.npy`、`gt_top20.npy` 和 `gt_top100.npy` MUST 都恰好有 1000 行
- **AND** 每个 GT 文件中的第 `i` 行 MUST 对应 `split_v1.query_indices[i]`
- **AND** GT dtype MUST 为 `int64`

#### Scenario: GT smoke detects mismatch

- **WHEN** 在合法 nprobe 下执行 recall smoke run
- **THEN** recall MUST 非零，且随 nprobe 增大保持预期的单调趋势
- **AND** 如果 recall 接近零，或 candidate recall 与当前 query split 不一致，验证流程 MUST 在正式 measurement 之前失败退出

### Requirement: Deep1M_synth deterministic synthetic payload distribution

系统 SHALL 使用确定性的 bucket-mixture 规则生成 `Deep1M_synth` 的 formal synthetic payload，并以 `cleaned/payload.parquet` 作为 payload backend 的唯一正式来源。

#### Scenario: Default payload distribution is fixed

- **WHEN** 生成 `deep1m_synth` 的 default payload variant
- **THEN** manifest MUST 记录 `payload_distribution_id=bucket_mixture_v1`
- **AND** manifest MUST 记录 `payload_seed=20260510`
- **AND** manifest MUST 记录 `min_bytes=256`、`target_mean_bytes=4096` 和 `max_bytes=65536`
- **AND** payload size buckets MUST 为 `256B:45.000%`、`1024B:25.000%`、`4096B:18.000%`、`16384B:9.922%` 和 `65536B:2.078%`
- **AND** 生成后的 payload size min MUST 为 `256`
- **AND** 生成后的 payload size max MUST 为 `65536`
- **AND** 生成后的实际 mean payload size MUST 与 `4096` bytes 的差值不超过 `1` byte

#### Scenario: Row payloads are deterministic

- **WHEN** 对任意 `row_id` 生成 payload
- **THEN** 其 payload size MUST 只由 `payload_distribution_id`、`payload_seed`、`row_id` 和固定 row count 决定
- **AND** 其 payload bytes MUST 只由 `dataset_id=deep1m_synth`、`row_id` 和 `payload_size` 决定
- **AND** 重复执行数据准备脚本 MUST 生成相同的 `payload_size` 序列和相同的抽样 payload checksum

#### Scenario: Cleaned payload is the source of truth

- **WHEN** 写出 `cleaned/payload.parquet`
- **THEN** schema MUST 至少包含 `row_id:int64`、`doc_id:int64`、`payload_size:int64` 和 `payload:large_binary`
- **AND** row count MUST 恰好为 `1,000,000`
- **AND** `row_id` 和 `doc_id` MUST 都与 base vector row id 对齐
- **AND** legacy `/home/zcq/VDB/baselines/data/deep1m_flatstor.*` MUST NOT 作为 formal payload source of truth
- **AND** FlatStor、Lance 和 Parquet backend MUST 从该 cleaned payload source 导出或逐字节等价派生

### Requirement: Deep1M_synth payload backend readiness

系统 SHALL 在正式 E2E 实验前，使 `deep1m_synth` 的 FlatStor 与 Lance 两种 payload backend 都可用。

#### Scenario: FlatStor default backend is ready

- **WHEN** payload 验证检查 `payload_flatstor/default`
- **THEN** `index.npy` MUST 存在，shape 为 `(1000000, 2)`
- **AND** `payload.dat` MUST 存在
- **AND** 随机抽取行做 payload fetch 时 MUST 返回正的字节数
- **AND** manifest MUST 记录这些文件由 `cleaned/payload.parquet` 的 `bucket_mixture_v1` synthetic payload 派生
- **AND** FlatStor 中随机抽样行的 payload size 与 bytes MUST 与 cleaned source 完全一致

#### Scenario: Lance default backend is ready

- **WHEN** payload 验证检查 `payload_lance/default`
- **THEN** MUST 存在一个与 `dataset_id=deep1m_synth` 兼容的 Lance table
- **AND** 该 table MUST 恰好包含 1,000,000 行
- **AND** 通过 E2E baseline 实际使用的同一条 fetch 路径做随机读取时 MUST 成功
- **AND** Lance 中随机抽样行的 payload size 与 bytes MUST 与 cleaned source 完全一致
- **AND** 如果当前配置的 runtime 环境不能导入 `lancedb`，验证流程 MUST 失败退出

### Requirement: Deep1M_synth loader support

Python formal-study runner SHALL 支持 `load_dataset_bundle("deep1m_synth")`。

#### Scenario: Dataset bundle loads with L2 semantics

- **WHEN** 调用 `load_dataset_bundle("deep1m_synth", query_limit=1000)`
- **THEN** 返回的 base vectors MUST 为 `(1000000, 96)`
- **AND** 返回的 query vectors MUST 为 `(1000, 96)`，且由持久化 split 选出
- **AND** 当 `gt_top100.npy` 存在时，返回的 GT MUST 至少有 100 列
- **AND** 返回的 metric MUST 为 `l2`
- **AND** 全部 1,000,000 个 base row 的 row ids MUST 为唯一的 `int64`

#### Scenario: Metric equivalence is documented

- **WHEN** 文档、manifest 或 validation report 描述 `deep1m_synth` 的度量设置
- **THEN** 它们 MUST 说明公开 Deep1M 向量已经做 L2 归一化
- **AND** 它们 MUST 说明本实验虽然按 `metric=l2` 执行，但欧氏距离（L2）排序与余弦相似度排序等价

### Requirement: Deep1M_synth canonical artifact readiness

系统 SHALL 为 VecFetch 与 baseline coarse reuse 生成一个可用的 `Deep1M_synth` canonical artifact。

#### Scenario: Canonical artifact manifest is valid

- **WHEN** 执行 canonical artifact validation
- **THEN** `/home/zcq/VDB/baselines/formal-study/outputs/index_build/deep1m_synth/<builder>/nlist4096_bits4_single/artifact_manifest.json` MUST 存在
- **AND** manifest MUST 记录 `dataset=deep1m_synth`、`nlist=4096`、`bits=4`、`assignment_mode=single` 和 `metric=l2`
- **AND** manifest MUST 指向真实存在的 `centroids.fvecs`、`assignments.ivecs`、`bench_dataset_dir`、`bench_index_dir` 和 `gt_top10_path`

#### Scenario: Canonical artifact reuses prepared clustering

- **WHEN** 使用已有 Deep1M clustering 构建 canonical artifact
- **THEN** 它 MUST 使用 `/home/zcq/VDB/data/deep1m/deep1m_centroid_4096.fvecs` 或其逐字节等价副本作为 centroids
- **AND** MUST 使用 `/home/zcq/VDB/data/deep1m/deep1m_cluster_id_4096.ivecs` 或其逐字节等价副本作为 assignments
- **AND** 除非命令显式要求 rebuild，否则它 MUST NOT 静默触发一次新的 clustering job

### Requirement: Deep1M_synth validation gate

系统 SHALL 提供一个 validation stage，在数据 contract 完整之前阻止 formal 实验继续执行。

#### Scenario: Validation passes

- **WHEN** formal vectors、split、GT、payload backend、loader support 和 canonical artifact 检查全部通过
- **THEN** validation MUST 写出 `deep1m_synth_asset_manifest.json`
- **AND** validation MUST 写出 `deep1m_synth_smoke_validation.md`
- **AND** 后续实验阶段 MAY 继续运行

#### Scenario: Validation fails

- **WHEN** 任何必需文件、shape、metric、payload 行数、Lance 环境、GT 对齐或 canonical artifact 检查失败
- **THEN** validation MUST 在写入 formal experiment output 之前停止
- **AND** failure report MUST 明确指出缺失或不一致的 contract item
