## 1. Formal Study Workspace Bootstrap

- [x] 1.1 Create `/home/zcq/VDB/baselines/formal-study/` workspace directories for `manifests/`, `scripts/`, `outputs/`, and `trackers/`
- [x] 1.2 Instantiate `dataset_registry.csv`, `encoder_registry.csv`, and `baseline_registry.csv` from the formal baseline study templates
- [x] 1.3 Instantiate `RUN_STATUS.csv` and `FAILURES.md` from the formal study tracking templates
- [x] 1.4 Add shared path and run-id conventions so every later script uses the same raw/formatted/output roots

## 2. Primary Dataset Preparation Pipeline

- [x] 2.1 Implement `prepare_datasets/coco_100k_manifest.py` to validate local COCO assets and write cleaned payload plus split manifest
- [x] 2.2 Implement `prepare_datasets/msmarco_download.sh` and `prepare_datasets/msmarco_prepare.py` to download, clean, and split MS MARCO Passage
- [x] 2.3 Implement `prepare_datasets/deep8m_link_or_copy.sh` and `prepare_datasets/deep8m_generate_synth_payload.py` for Deep8M raw assets and deterministic payload tiers
- [x] 2.4 Implement `prepare_datasets/esci_download.sh` and `prepare_datasets/esci_prepare.py` to download, clean, and split Amazon ESCI
- [x] 2.5 Add dataset-level sanity checks that verify required raw files, cleaned outputs, and split manifests before a dataset is marked runnable

## 3. Embedding And Ground Truth Generation

- [x] 3.1 Implement or wire up embedding builders for COCO, MS MARCO, and Amazon ESCI using frozen encoder registry entries
- [x] 3.2 Implement `build_groundtruth/coco_100k_gt.py`, `msmarco_gt.py`, `esci_gt.py`, and `deep8m_convert_gt.py`
- [x] 3.3 Preserve task labels under the dataset `gt/` roots for labeled datasets such as `msmarco_passage` and `amazon_esci`
- [x] 3.4 Add validation that embedding metric, normalization, and generated `gt_top10.npy` / `gt_top20.npy` match the dataset registry contract
- [x] 3.5 Extend the ground-truth contract to generate and validate `gt_top100.npy` for every primary dataset so the `topk=100` main experiment is evaluable

## 4. Payload Backend Export

- [x] 4.1 Implement payload export scripts for COCO, MS MARCO, and Amazon ESCI to emit aligned `FlatStor`, `Lance`, and `Parquet` backends
- [x] 4.2 Implement Deep8M size-tiered backend export for `256B`, `4KB`, and `64KB` payload families
- [x] 4.3 Add schema-alignment checks so all exported backends preserve the same logical field set per dataset
- [x] 4.4 Record backend output size, row count, and path metadata back into the formal study manifests

## 5. Baseline System Integration

- [x] 5.1 Integrate `Faiss IVFPQ + rerank` under the shared vector-search runner contract with disk-backed rerank
- [x] 5.2 Keep `Faiss IVFPQR` only as a diagnostic runner and exclude it from the phase-1 mandatory comparison contract
- [x] 5.3 Integrate `Faiss IVFResidualQuantizer` or `IVFLocalSearchQuantizer` as a legacy reference path (`faiss_ivfrq`)
- [x] 5.4 Run the DiskANN thin-wrapper smoke gate and mark it `active` or `blocked` in the baseline registry without blocking phase-1 closure
- [x] 5.5 Register `BoundFetch`, `ConANN + Faiss`, and `Extended-RaBitQ` as deferred or optional entries without implementing them in phase 1

## 6. Coupled E2E And Benchmark Output Contract

- [x] 6.1 Implement formal-study coupled E2E runners for the active non-BoundFetch baseline family without modifying `benchmarks/bench_e2e.cpp`
- [x] 6.2 Standardize per-run metric fields for vector-search, coupled E2E, payload microbench, and build/startup outputs
- [x] 6.3 Implement payload microbench runners that write only to `outputs/payload_microbench/` and never override formal E2E outputs
- [x] 6.4 Add dataset-level E2E summary generation at `outputs/e2e/{dataset}/e2e_system_summary.csv`

## 7. Result Aggregation And Tracking

- [x] 7.1 Update `RUN_STATUS.csv` automatically for every scheduled, running, completed, failed, and retried run
- [x] 7.2 Append structured failure records to `FAILURES.md` with dataset, system, config, and log path
- [x] 7.3 Implement formal-study aggregation for per-system summary CSVs, build/startup tables, and plot-ready comparison tables
- [x] 7.4 Add controlled backfill from `/home/zcq/VDB/baselines/formal-study/outputs/` into `/home/zcq/VDB/baselines/results/` and the associated analysis documents

## 8. Batch Execution And Validation

- [x] 8.1 Run the first batch on `COCO 100K` for the mandatory non-BoundFetch baseline suite and at least one primary payload backend
- [x] 8.2 Prepare and validate `MS MARCO Passage` for baseline-ready search, coupled E2E, and label-aware outputs
- [x] 8.3 Prepare and validate `Deep8M-synth` across the three payload-size tiers and storage/backend comparisons
- [x] 8.4 Prepare and validate `Amazon ESCI` for baseline-ready search, coupled E2E, and label-aware outputs
- [x] 8.5 Materialize appendix dataset manifests and sanity-run hooks for `LAION subset`, `Clotho`, and `MSR-VTT`, gated behind the primary suite
- [x] 8.6 Freeze the main-experiment search controls with `topk ∈ {10,50,100}`, `candidate_budget ∈ {100,250,500}` mapped to `topk`, `FlatStor` as the primary backend, and dataset-specific `nlist` values (`COCO=2048`, `Deep8M=12800`, `MS MARCO=16384`, `Amazon ESCI=8192` unless cardinality review changes it)

### 8.7 COCO 100K Main Experiment

- [x] 8.7.1 Run `coco_100k` gate vector-search and coupled E2E checks before the full sweep
- [x] 8.7.2 Run `coco_100k × faiss_ivfpq_refine × topk=10` on the frozen `nprobe` grid
- [x] 8.7.3 Run `coco_100k × faiss_ivfrq × topk=10` on the frozen `nprobe` grid (deprecated: `faiss_ivfrq` retained for historical reference only)
- [x] 8.7.4 Run `coco_100k × diskann × topk=10` on the frozen `L_search` grid if DiskANN remains active
- [x] 8.7.4a Immediately aggregate the completed `coco_100k × topk=10` slice and generate its Pareto plot before proceeding
- [x] 8.7.5 Run `coco_100k × faiss_ivfpq_refine × topk=50` on the frozen `nprobe` grid
- [x] 8.7.6 Run `coco_100k × faiss_ivfrq × topk=50` on the frozen `nprobe` grid (deprecated: `faiss_ivfrq` retained for historical reference only)
- [x] 8.7.7 Run `coco_100k × diskann × topk=50` on the frozen `L_search` grid if DiskANN remains active
- [x] 8.7.7a Immediately aggregate the completed `coco_100k × topk=50` slice and generate its Pareto plot before proceeding
- [x] 8.7.8 Run `coco_100k × faiss_ivfpq_refine × topk=100` on the frozen `nprobe` grid
- [x] 8.7.9 Run `coco_100k × faiss_ivfrq × topk=100` on the frozen `nprobe` grid (deprecated: `faiss_ivfrq` retained for historical reference only)
- [x] 8.7.10 Run `coco_100k × diskann × topk=100` on the frozen `L_search` grid if DiskANN remains active
- [x] 8.7.10a Immediately aggregate the completed `coco_100k × topk=100` slice and generate its Pareto plot before proceeding
- [x] 8.7.11 Merge the three `coco_100k` top-k slices into a dataset-level main-experiment summary
- [x] 8.7.12 Write a short `coco_100k` result report and reference it from the formal-study outputs

### 8.8 MS MARCO Passage Main Experiment

- [x] 8.8.1 Run `msmarco_passage` gate vector-search and coupled E2E checks before the full sweep
- [x] 8.8.2 Fix and validate query/ground-truth/index alignment if the gate run fails
- [x] 8.8.3 Run `msmarco_passage × faiss_ivfpq_refine × topk=10` on the frozen `nprobe` grid
- [x] 8.8.4 Run `msmarco_passage × faiss_ivfrq × topk=10` on the frozen `nprobe` grid (deprecated: `faiss_ivfrq` retained for historical reference only)
- [x] 8.8.4a Immediately aggregate the completed `msmarco_passage × topk=10` slice and generate its Pareto plot before proceeding
- [x] 8.8.5 Run `msmarco_passage × faiss_ivfpq_refine × topk=50` on the frozen `nprobe` grid
- [x] 8.8.6 Run `msmarco_passage × faiss_ivfrq × topk=50` on the frozen `nprobe` grid (deprecated: `faiss_ivfrq` retained for historical reference only)
- [x] 8.8.6a Immediately aggregate the completed `msmarco_passage × topk=50` slice and generate its Pareto plot before proceeding
- [x] 8.8.7 Run `msmarco_passage × faiss_ivfpq_refine × topk=100` on the frozen `nprobe` grid
- [x] 8.8.8 Run `msmarco_passage × faiss_ivfrq × topk=100` on the frozen `nprobe` grid (deprecated: `faiss_ivfrq` retained for historical reference only)
- [x] 8.8.8a Immediately aggregate the completed `msmarco_passage × topk=100` slice and generate its Pareto plot before proceeding
- [x] 8.8.9 Merge the three `msmarco_passage` top-k slices into a dataset-level main-experiment summary
- [x] 8.8.10 Write a short `msmarco_passage` result report

### 8.9 Deep8M-synth Main Experiment

- [x] 8.9.1 Run `deep8m_synth` gate vector-search and coupled E2E checks for the `256B` payload tier (`flatstor`/`topk=10` 已完成)
- [x] 8.9.2 Run `deep8m_synth` main sweep for the `256B` payload tier across `topk=10/50/100` (`flatstor`/`protocol warm` 已完成)
- [x] 8.9.2a Immediately aggregate each completed `deep8m_synth × 256B × topk` slice and generate its Pareto plot (`flatstor` 已产出)
- [x] 8.9.3 Run `deep8m_synth` gate vector-search and coupled E2E checks for the `4KB` payload tier
- [x] 8.9.4 Run `deep8m_synth` main sweep for the `4KB` payload tier across `topk=10/50/100`
- [x] 8.9.4a Immediately aggregate each completed `deep8m_synth × 4KB × topk` slice and generate its Pareto plot
- [x] 8.9.5 Run `deep8m_synth` gate vector-search and coupled E2E checks for the `64KB` payload tier
- [x] 8.9.6 Run `deep8m_synth` main sweep for the `64KB` payload tier across `topk=10/50/100`
- [x] 8.9.6a Immediately aggregate each completed `deep8m_synth × 64KB × topk` slice and generate its Pareto plot
- [x] 8.9.7 Merge all `deep8m_synth` payload-tier and top-k slices into a dataset-level main-experiment summary
- [x] 8.9.8 Write a short `deep8m_synth` result report

### 8.10 Amazon ESCI Main Experiment

- [ ] 8.10.1 Run `amazon_esci` gate vector-search and coupled E2E checks before the full sweep
- [ ] 8.10.2 Validate label-aware outputs and fix query/ground-truth/index alignment if the gate run fails
- [x] 8.10.3 Run `amazon_esci × faiss_ivfpq_refine × topk=10` on the frozen `nprobe` grid
- [x] 8.10.4 Run `amazon_esci × faiss_ivfrq × topk=10` on the frozen `nprobe` grid (deprecated: `faiss_ivfrq` retained for historical reference only)
- [x] 8.10.4a Immediately aggregate the completed `amazon_esci × topk=10` slice and generate its Pareto plot before proceeding
- [x] 8.10.5 Run `amazon_esci × faiss_ivfpq_refine × topk=50` on the frozen `nprobe` grid
- [x] 8.10.6 Run `amazon_esci × faiss_ivfrq × topk=50` on the frozen `nprobe` grid (deprecated: `faiss_ivfrq` retained for historical reference only)
- [x] 8.10.6a Immediately aggregate the completed `amazon_esci × topk=50` slice and generate its Pareto plot before proceeding
- [x] 8.10.7 Run `amazon_esci × faiss_ivfpq_refine × topk=100` on the frozen `nprobe` grid
- [x] 8.10.8 Run `amazon_esci × faiss_ivfrq × topk=100` on the frozen `nprobe` grid (deprecated: `faiss_ivfrq` retained for historical reference only)
- [x] 8.10.8a Immediately aggregate the completed `amazon_esci × topk=100` slice and generate its Pareto plot before proceeding
- [x] 8.10.9 Merge the three `amazon_esci` top-k slices into a dataset-level main-experiment summary
- [x] 8.10.10 Write a short `amazon_esci` result report

### 8.11 Main Experiment Aggregation

- [x] 8.11.1 Merge the already-emitted dataset-local `topk=10` summaries into a unified main-suite table
- [x] 8.11.2 Merge the already-emitted dataset-local `topk=50` summaries into a unified main-suite table
- [x] 8.11.3 Merge the already-emitted dataset-local `topk=100` summaries into a unified main-suite table
- [x] 8.11.4 Generate only global main-suite comparison figures from the merged summaries, without replacing the dataset-local Pareto plots
- [x] 8.11.5 Write the main-suite narrative report using the dataset-level reports as inputs

### 8.12 Extended Experiment Operating Point Selection

- [x] 8.12.1 Select `coco_100k` top-10 operating points at `recall@10 ≈ 0.80 / 0.90 / 0.95`
- [x] 8.12.2 Select `msmarco_passage` top-10 operating points at `recall@10 ≈ 0.80 / 0.90 / 0.95`
- [x] 8.12.3 Select `deep8m_synth` top-10 operating points at `recall@10 ≈ 0.80 / 0.90 / 0.95`
- [x] 8.12.4 Select `amazon_esci` top-10 operating points at `recall@10 ≈ 0.80 / 0.90 / 0.95`
- [x] 8.12.5 Write a unified operating-point manifest with `matched`, `best-effort`, and `unreached` states

### 8.13 Extended Experiment Backend Replay

- [x] 8.13.1 Replay `FlatStor / Lance / Parquet` on the selected `coco_100k` top-10 operating points
- [x] 8.13.2 Replay `FlatStor / Lance / Parquet` on the selected `msmarco_passage` top-10 operating points
- [x] 8.13.3 Replay `FlatStor / Lance / Parquet` on the selected `deep8m_synth` top-10 operating points
- [x] 8.13.4 Replay `FlatStor / Lance / Parquet` on the selected `amazon_esci` top-10 operating points

### 8.14 Extended Experiment Aggregation

- [x] 8.14.1 Aggregate dataset-level backend comparison summaries
- [x] 8.14.2 Generate backend comparison plot-ready CSVs and figures
- [x] 8.14.3 Write the backend comparison narrative report

### 8.15 Appendix Gate

- [x] 8.15.1 Verify that all main-experiment dataset blocks are complete
- [x] 8.15.2 Verify that all backend extension dataset blocks are complete
- [x] 8.15.3 Gate appendix-dataset execution on successful completion of the main and extended experiment suites
