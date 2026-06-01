## 1. 数据合约与资产准备

- [x] 1.1 创建 `Deep1M_synth` 的 formal raw 根目录 `/home/zcq/VDB/data/formal_baselines/deep1m_synth/embeddings/`，并从 `/home/zcq/VDB/data/deep1m` link 或 copy `base_embeddings.npy` 与 `query_embeddings.npy`
- [x] 1.2 创建 `Deep1M_synth` 的 formatted 根目录 `/home/zcq/VDB/baselines/data/formal_baselines/deep1m_synth/`，补齐 `cleaned/`、`gt/`、`splits/`、`payload_flatstor/`、`payload_lance/` 与 `payload_parquet/` 等子目录
- [x] 1.3 使用 1000-query 的 `sample_query_indices(10000, 1000)` 约定生成并持久化 `splits/split_v1.json`
- [x] 1.4 根据持久化 split 生成 formal `gt_top10.npy`、`gt_top20.npy` 和 `gt_top100.npy`，并统一转成 `int64`
- [x] 1.5 为 `Deep1M_synth` 的 default payload contract 生成首版最小 `cleaned/payload.parquet`；该首版 legacy-based payload 已被新的 3R synthetic payload remediation 取代，正式实验前必须按 3R.1-3R.4 重新生成
- [x] 1.6 写出 `Deep1M_synth` 数据 manifest，记录 source paths、split checksum、GT shapes、payload variant、payload source 和 row counts；正式实验前必须按 3R.4 更新为 `bucket_mixture_v1` provenance

## 2. Payload Backend Formalization

- [x] 2.1 完成首版 legacy FlatStor index 接入；该文件只保留为迁移参考，正式 `payload_flatstor/default/index.npy` 必须按 3R.5 从 cleaned synthetic payload 重新导出
- [x] 2.2 完成首版 legacy FlatStor payload 接入；该文件只保留为迁移参考，正式 `payload_flatstor/default/payload.dat` 必须按 3R.5 从 cleaned synthetic payload 重新导出
- [x] 2.3 准备 `payload_lance/default`，确保其中的 table 与 `dataset_id=deep1m_synth` 兼容；正式 Lance backend 必须按 3R.6 从 cleaned synthetic payload 重导出，而不是继续依赖 legacy 表名 `deep1m`
- [x] 2.4 验证 Lance 所用 Python runtime 能导入 `lancedb`，并记录是否必须使用 `/home/zcq/anaconda3/envs/labnew/bin/python`
- [x] 2.5 验证 FlatStor 与 Lance 的 row count 都恰好为 1,000,000，并通过 E2E runner 实际使用的 fetch helper 执行随机 payload fetch smoke check

## 3. Runner 与 Adapter 支持

- [x] 3.1 在 `_shared.datasets.load_dataset_bundle` 中新增显式的 `deep1m_synth` 支持，固定使用 `l2` metric、formal split、`gt_top100` 优先读取与 `0..999999` row ids，并在 manifest/validation 文档中注明“公开 Deep1M 已做 L2 归一化，因此 L2 与 cosine 排序等价”
- [x] 3.2 更新 `dataset_runnable_summary` 与 dataset validation 逻辑，使 `Deep1M_synth` 只有在 formal vectors、split、GT、payload backend 和 canonical artifact 全部就绪时才报告 `phase0_ready=True`
- [x] 3.3 在 payload export 或 payload validation 路径中加入 `Deep1M_synth` 支持，覆盖 default variant 和 Lance table naming contract
- [x] 3.4 创建 `Deep1M_synth` 的 `bench_e2e` adapter，补齐 `image_embeddings.npy`、`image_ids.npy`、`query_embeddings.npy`、`query_ids.npy` 和 `metadata.jsonl`
- [x] 3.5 确保 baseline vector-search 与 coupled E2E 脚本可以接受 `deep1m_synth`，且不会误走 cosine-only dataset 逻辑

## 3R. Synthetic Payload Remediation

- [x] 3R.1 更新 `prepare_deep1m_synth_assets.py`，把 default payload variant 从 legacy FlatStor bytes 改为 deterministic `bucket_mixture_v1` 生成器，固定 `payload_seed=20260510`、min=256B、target mean=4KB、max=64KB
- [x] 3R.2 使用 exact-count seeded shuffle 生成 1,000,000 行 payload size assignment，bucket 为 `256B:45.000%`、`1024B:25.000%`、`4096B:18.000%`、`16384B:9.922%`、`65536B:2.078%`
- [x] 3R.3 重新生成 `cleaned/payload.parquet`，schema 至少包含 `row_id:int64`、`doc_id:int64`、`payload_size:int64` 和 `payload:large_binary`，并确保 legacy `deep1m_flatstor.*` 不再作为 formal source of truth
- [x] 3R.4 更新 `dataset_manifest.json`、`backend_export_default.json` 和 sanity report，记录 distribution id、seed、bucket counts、actual min/mean/max、total bytes、payload_size checksum 和样本 payload checksum
- [x] 3R.5 从新的 `cleaned/payload.parquet` 重新导出 `payload_flatstor/default/index.npy` 与 `payload.dat`，并验证随机样本的 payload size 与 bytes 与 cleaned source 完全一致
- [x] 3R.6 从新的 `cleaned/payload.parquet` 重新导出 `payload_lance/default`，保持 table 与 `dataset_id=deep1m_synth` 兼容，并验证 row count、doc id、payload size 与 bytes
- [x] 3R.7 从新的 `cleaned/payload.parquet` 生成或验证 `payload_parquet/default`，确保后续 exporter/loader 能读取 `payload_size` 元数据
- [x] 3R.8 在 validation stage 中加入 payload distribution gate：若 `payload_distribution_id`、bucket counts、min/max/mean 或抽样 bytes checksum 不匹配，则禁止进入 canonical artifact 和正式 sweep

## 4. Canonical Artifact 与 Index Readiness

- [x] 4.1 新增一个 `Deep1M_synth` canonical artifact entrypoint，或扩展现有 canonical build 脚本以接受 `deep1m_synth`
- [x] 4.2 复用 `/home/zcq/VDB/data/deep1m/deep1m_centroid_4096.fvecs` 与 `/home/zcq/VDB/data/deep1m/deep1m_cluster_id_4096.ivecs` 作为 canonical 的 4096 single-assignment coarse files
- [x] 4.3 使用相同的 centroids / assignments 构建或解析 `bench_e2e` index 目录，参数固定为 `nlist=4096`、`bits=4`、`assignment_mode=single`、`metric=l2`
- [x] 4.4 在 `/home/zcq/VDB/baselines/formal-study/outputs/index_build/deep1m_synth/<builder>/nlist4096_bits4_single/` 下写出 `artifact_manifest.json`
- [x] 4.5 验证 VecFetch、IVF+PQ 和 IVF+RaBitQ runner 都能加载该 canonical artifact，且不会静默重建另一套 clustering

## 5. Validation 与 Smoke Runs

- [x] 5.1 实现或固化一个 `validate` stage，统一检查 formal vector shapes、split、GT 对齐、payload backend、Lance 环境、loader 行为和 canonical artifact paths，并输出 L2 与 cosine 等价的说明
- [x] 5.2 调用 `load_dataset_bundle("deep1m_synth", query_limit=1000)`，验证 base shape、query shape、GT shape、row ids 与 `metric=l2`
- [x] 5.3 用 canonical artifact 和 external GT 跑一个 `VecFetch topk=10,nprobe=64` 的 smoke point
- [x] 5.4 跑一个 `IVF+RaBitQ+FlatStor topk=10,nprobe=64,candidate_budget=100` smoke point
- [x] 5.5 跑一个 `IVF+PQ+FlatStor topk=10,nprobe=64,candidate_budget=100` smoke point
- [x] 5.6 跑一个 `IVF+RaBitQ+Lance topk=10,nprobe=64,candidate_budget=100` smoke point
- [x] 5.7 跑一个 `IVF+PQ+Lance topk=10,nprobe=64,candidate_budget=100` smoke point
- [x] 5.8 写出 `deep1m_synth_asset_manifest.json` 和 `deep1m_synth_smoke_validation.md`；如果任一 smoke recall 接近零，或 payload fetch 失败，则阻止 formal sweep 继续

## 6. Topk=10 主 Sweep

- [x] 6.1 跑 `VecFetch / BoundFetch-Guarded topk=10` sweep，nprobe 取 `16,32,64,128,256,512`；在启动正式 sweep 前只对 VecFetch 做 1 次 warmup，warmup 不入表，后续各 nprobe 点直接执行 measurement
- [x] 6.2 跑 `IVF+RaBitQ+FlatStor topk=10` sweep，nprobe 取 `16,32,64,128,256,512`，`candidate_budget=100`，不做额外 warmup
- [x] 6.3 跑 `IVF+PQ+FlatStor topk=10` sweep，nprobe 取 `16,32,64,128,256,512`，`candidate_budget=100`，不做额外 warmup
- [x] 6.4 跑 `IVF+RaBitQ+Lance topk=10` sweep，nprobe 取 `16,32,64,128,256,512`，`candidate_budget=100`，不做额外 warmup
- [x] 6.5 跑 `IVF+PQ+Lance topk=10` sweep，nprobe 取 `16,32,64,128,256,512`，`candidate_budget=100`，不做额外 warmup
- [x] 6.6 生成 `deep1m_synth_main_sweep_top10.csv` 与 `deep1m_synth_recall_latency_curve_top10.csv`

## 7. Matched-Quality 选点与 Topk=20 Supplement

- [x] 7.1 先用 common-threshold 规则做 `topk=10` matched-quality 选点；只有失败时才回退到 narrow-band 规则
- [x] 7.2 写出 `deep1m_synth_matched_quality_top10.csv`，记录每个保留行的选点规则、recall、latency、backend、candidate budget 和 provenance
- [x] 7.3 在启动 `topk=20` formal run 之前，先确认 runner 与 summarizer 能输出有效的 `recall@20`
- [x] 7.4 在选中的低/中/高质量区间运行 VecFetch `topk=20`，初始点位取 `32,64,128` 或 `64,128,256`
- [x] 7.5 在同一质量区间运行四个 IVF baseline 组合的 `topk=20`，`candidate_budget` 从 `150` 开始，只有 candidate recall 不足时才回退到 `200`
- [x] 7.6 如果 `topk=20` 顺利完成，生成 `deep1m_synth_topk20_supplement.csv` 与 `deep1m_synth_topk10_vs_topk20_summary.csv`

## 8. Cleanup、Aggregation 与 Reporting

- [x] 8.1 不再对所有最终可见的 `topk=10` 点执行统一 warmup/repeat cleanup；`topk=10` 正式结果直接来自 sweep measurement，唯一 warmup 是 6.1 中 VecFetch sweep 前的 1 次预热
- [x] 8.2 如果 `topk=20` 被用于报告，同样不做统一 warmup/repeat cleanup；仅在启动 VecFetch `topk=20` sweep 前做 1 次 warmup，baseline 不做额外 warmup
- [x] 8.3 生成 `deep1m_synth_sweep_summary.csv`，包含各系统 sweep measurement 的 recall、latency、backend、candidate budget 和 provenance 字段
- [x] 8.4 生成 `DEEP1M_SYNTH_DECISION_SUMMARY.md`，记录最终选点、baseline completeness、threshold/fallback rule、`topk=20` 状态以及任何 blocked rows
- [x] 8.5 在对应任务完成后，更新 `Deep1M_synth` experiment tracker 中 `D1M-000` 到 `D1M-050` 的状态
- [x] 8.6 保持 failed、smoke、warmup 和 formal measurement outputs 分目录存放，并确保 failed/smoke 行不会混入论文 CSV
