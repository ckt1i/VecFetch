# Implementation Notes

## 2026-06-02T01:56:12+0800 Safety Baseline

- Disk usage:
  - `/home/zcq/VDB`: filesystem `/dev/nvme0n1p2`, size `3.5T`, used `1.8T`, available `1.5T`, use `55%`.
  - `/home/zcq/VDB/data`: `222G`.
  - `/home/zcq/VDB/baselines/data`: `146G`.
  - `/home/zcq/VDB/baselines/formal-study/outputs`: `77G`.
- Raw dataset footprints:
  - `/home/zcq/VDB/data/raw_downloads/imagenet1k_official`: `145G`.
  - `/home/zcq/VDB/data/raw_downloads/voxceleb2_dev_wds`: `12G`.
  - `/home/zcq/VDB/data/formal_baselines/imagenet1k`: `2.9G`.
  - `/home/zcq/VDB/data/formal_baselines/voxceleb2_ecapa_150k`: `173M`.
- Active-process check:
  - No active `run_icde_baseline_suite`, `run_diskann`, `build_disk_index`, `search_disk_index`, FAISS baseline, ImageNet/Vox embedding, or `prepare_dataset_expansion` process was observed.
  - Only background editor/system services and `code_review_graph serve` were present.
- Raw embedding state:
  - `imagenet1k/base_embeddings.npy`: shape `(1281167, 512)`, `float32`, bytes `2623830144`.
  - `imagenet1k/query_embeddings.npy`: shape `(50000, 512)`, `float32`, bytes `102400128`.
  - ImageNet metadata: `encoder_id=clip_vit_b32_image`, `model_name_or_path=openai/clip-vit-base-patch32`, `metric=cosine`, `normalization=l2`, `source_format=official_imagenet_2012_tar`.
  - `voxceleb2_ecapa_150k/base_embeddings.npy`: shape `(150000, 192)`, `float32`, bytes `115200128`.
  - `voxceleb2_ecapa_150k/query_embeddings.npy`: shape `(10000, 192)`, `float32`, bytes `7680128`.
  - Vox metadata: `encoder_id=speechbrain_ecapa_voxceleb`, `model_name_or_path=speechbrain/spkrec-ecapa-voxceleb`, `metric=cosine`, `normalization=l2`, `total_valid=160000`, `base_rows=150000`, `query_rows=10000`.

## Safety Rules For This Change

- Reuse existing raw archive files and existing raw embedding files whenever possible.
- Do not unpack ImageNet or VoxCeleb2 into duplicate full raw file trees.
- Stop before long materialization or experiment stages if `/home/zcq/VDB` free space falls below the configured guard.
- Do not delete user-provided raw downloads, prior experiment results, or existing indexes.
- Only generated files from this change may be replaced by this change's scripts.

## 2026-06-02 VoxCeleb2 IVF Progress

- `voxceleb2_ecapa_150k` phase-0 readiness passed with required payload backends `flatstor,lance`; `payload_parquet` is absent and not required.
- Vox payload exports:
  - FlatStor rows `150000`, payload bytes `10715021874`.
  - Lance rows `150000`, payload bytes `10715021874`.
- Vox exact GT:
  - `gt_top10.npy`: `(1000, 10)`.
  - `gt_top20.npy`: `(1000, 20)`.
  - `gt_top100.npy`: `(1000, 100)`.
- Vox smoke results passed for:
  - `IVF+PQ+FlatStor`, `IVF+PQ+Lance` at `topk=10`, `nprobe=8`.
  - `IVF+RaBitQ+FlatStor`, `IVF+RaBitQ+Lance` at `topk=10`, `nprobe=8`, with shared vector-search output.
- Vox full IVF baseline sweep completed:
  - `48` `icde_baseline` E2E rows.
  - Systems: `24` `faiss_ivfpq_refine`, `24` `ivf_rabitq_rerank`.
  - Backends: `24` `flatstor`, `24` `lance`.
  - Top-k: `24` rows for `10`, `24` rows for `50`.
  - Parquet rows: `0`.
  - Top-k 20 rows: `0`.
  - FlatStor/Lance parameter parity: passed for IVF+PQ and IVF+RaBitQ.
- Vox IVF+RaBitQ recall ranges for DiskANN coverage gate:
  - `topk=10`: `0.944100` to `0.997400`.
  - `topk=50`: `0.880340` to `0.992440`.
- Disk usage after Vox IVF:
  - `/home/zcq/VDB` available: about `1.5T`, use `56%`.
  - Vox E2E outputs: `233M`.
  - Vox vector-search outputs: `59M`.
  - Vox RaBitQ index cache: `150M`.
  - Vox IVFPQ index cache: `13M`.

## 2026-06-02 VoxCeleb2 DiskANN Promotion

- Fixed DiskANN runner query loading for formatted split datasets: when `gt_top100.npy` rows match `splits/split_v1.json`, queries are loaded by `query_indices[:N]` and GT by the corresponding leading rows.
- Discarded the initial R32/L50 probe for decision-making because it used misaligned query/GT rows and produced near-zero recall.
- Probe summary against Vox IVF+RaBitQ ranges:
  - RaBitQ `topk=10`: `0.944100` to `0.997400`.
  - RaBitQ `topk=50`: `0.880340` to `0.992440`.
  - R32/L50 raw covered `topk=10` but failed `topk=50` low-end coverage (`min=0.963180`).
  - R16/L50 and R8/L50 did not satisfy both low and high endpoints.
  - R12/L100 covered `topk=50` but missed `topk=10` high-end coverage.
  - Accepted index: `diskann_disk_voxceleb2_ecapa_150k_R13_L100_PQ0`.
- Accepted R13/L100 probe coverage:
  - `topk=10`: `0.691100` to `0.997700`.
  - `topk=50`: `0.878800` to `0.996380`.
- Promoted Vox DiskANN rows to `/home/zcq/VDB/baselines/formal-study/outputs/icde_baselines/diskann_icde_baselines.csv` after backing up the previous CSV:
  - Backup: `backups/diskann_icde_baselines_before_vox_r13_l100_20260602_022722.csv`.
  - Promoted rows: `28` valid rows.
  - Backends: `flatstor,lance`.
  - Top-k: `10,50`.
  - Unique index id: `diskann_disk_voxceleb2_ecapa_150k_R13_L100_PQ0`.
- Aggregated summary after promotion:
  - Summary rows for Vox: `76`.
  - Systems: `DiskANN`, `IVF+PQ`, `IVF+RQ`.
  - Backends: `flatstor,lance`; no Parquet rows.
  - Top-k: `10,50`; no `topk=20` rows.
  - Summary backup: `backups/icde_baseline_summary_before_aggregate_20260602_033955.csv`.
- Disk usage after Vox DiskANN promotion:
  - `/home/zcq/VDB` available: about `1.5T`, use `56%`.
  - Accepted Vox DiskANN index: `406M`.
  - ICDE baseline output directory: `159M`.

## 2026-06-02 ImageNet Payload, GT, And IVF Progress

- ImageNet payload export completed without generating Parquet:
  - FlatStor rows: `1281167`, payload bytes: `146907888532`.
  - Lance rows: `1281167`, payload bytes: `146907888532`.
  - Formal ImageNet directory size after export: about `274G`.
- ImageNet exact GT completed:
  - `gt_top10.npy`: `(1000, 10)`.
  - `gt_top20.npy`: `(1000, 20)`.
  - `gt_top100.npy`: `(1000, 100)`.
- ImageNet phase-0 readiness passed with required payload backends `flatstor,lance`.
- ImageNet full IVF baseline sweep completed:
  - `48` `icde_baseline` E2E rows.
  - Systems: `24` `faiss_ivfpq_refine`, `24` `ivf_rabitq_rerank`.
  - Backends: `24` `flatstor`, `24` `lance`.
  - Top-k: `24` rows for `10`, `24` rows for `50`.
  - Parquet rows: `0`.
  - Top-k 20 rows: `0`.
  - FlatStor/Lance parameter parity: passed for IVF+PQ and IVF+RaBitQ.
- ImageNet IVF+RaBitQ recall ranges for DiskANN coverage gate:
  - `topk=10`: `0.925500` to `0.999400`.
  - `topk=50`: `0.888340` to `0.999240`.
- Disk usage after ImageNet IVF:
  - `/home/zcq/VDB` available: about `1.2T`, use `64%`.
  - ImageNet E2E outputs: `232M`.
  - ImageNet vector-search outputs: `59M`.

## 2026-06-02T11:49:47+0800 ImageNet DiskANN Probe Status

- ImageNet IVF+RaBitQ reference ranges for the DiskANN single-index coverage gate:
  - `topk=10`: `0.925500` to `0.999400`.
  - `topk=50`: `0.888340` to `0.999240`.
- Probe rows remained isolated under `/home/zcq/VDB/baselines/formal-study/outputs/icde_baselines/probes/`; no ImageNet DiskANN rows were promoted to the main ICDE baseline CSV.
- Best candidate by total coverage miss was the original `diskann_disk_imagenet1k_R13_L100_PQ0`:
  - `topk=10`: `0.709500` to `0.999100`; low endpoint passed, high endpoint missed by `0.000300`.
  - `topk=50`: `0.881320` to `0.999280`; both endpoints passed.
  - Extra no-rebuild high-search probes at `L_search=16384` and `32768`, `beam=16`, did not improve the `topk=10` high endpoint; both stayed at `0.999100`.
- Additional rejected candidates:
  - `diskann_disk_imagenet1k_R14_L50_PQ0`: `topk=10` reached only `0.999200`; `topk=50` low point was `0.888540`, missing the low endpoint by `0.000200`. `search_io_limit=1` did not lower it.
  - `diskann_disk_imagenet1k_R13_L100_B64_PQ0`: rebuilt with `search_mem_gb=64`; `topk=10` reached only `0.999000`.
  - `diskann_disk_imagenet1k_R13_L100_T16_PQ0`: rebuilt with `build_threads=16`; `topk=10` reached only `0.999000`.
  - `diskann_disk_imagenet1k_R32_L100_PQ0`: `topk=10` missed high endpoint by `0.000100`, but `topk=50` low endpoint was too high by `0.076000`.
  - `diskann_disk_imagenet1k_R40_L100_PQ0`: `topk=10` covered both endpoints, but `topk=50` low endpoint was too high by `0.084260`; `search_io_limit` variants did not lower it.
- Current decision:
  - No tested ImageNet DiskANN index satisfies the required single-index recall-range coverage for both `topk=10` and `topk=50`.
  - ImageNet DiskANN promotion remains blocked; task 7.12 is intentionally left incomplete.
  - Main result tables continue to exclude ImageNet DiskANN probe rows until an accepted single index is found.
- Disk usage after the latest probes:
  - `/home/zcq/VDB` available: about `1011G`, use `70%`.
  - New generated ImageNet DiskANN probe indexes from this pass were not deleted.

## 2026-06-02T13:24:43+0800 ImageNet DiskANN Raw-Vector Correction

- Correction applied: DiskANN promoted/probe search for ImageNet must use raw SSD vectors only; compressed SSD-vector PQ indexes are out of scope for this baseline.
- Stopped the interrupted `diskann_disk_imagenet1k_R64_L100_PQ8` build process from the previous turn before deleting files.
- Deleted only generated ImageNet DiskANN PQ index directories:
  - `/home/zcq/VDB/baselines/data/diskann_disk_imagenet1k_R64_L100_PQ8_REORDER`.
  - `/home/zcq/VDB/baselines/data/diskann_disk_imagenet1k_R64_L100_PQ8`.
  - `/home/zcq/VDB/baselines/data/diskann_disk_imagenet1k_R64_L100` (`diskann_manifest.json` recorded `pq_disk_bytes=32` even though the directory name had no PQ suffix).
- Verified by scanning ImageNet DiskANN manifests that no remaining ImageNet DiskANN index directory has `pq_disk_bytes > 0`.
- Reverted the temporary `--append-reorder-data` / `--use-reorder-data` runner changes so subsequent runs cannot accidentally use the rejected PQ+reorder path.
- Rebuilt a fresh raw-vector ImageNet DiskANN index with:
  - Index id: `diskann_disk_imagenet1k_R13_L100_RAW_REBUILD_PQ0`.
  - Build command used `--PQ_disk_bytes 0`.
  - Output path: `/home/zcq/VDB/baselines/data/diskann_disk_imagenet1k_R13_L100_RAW_REBUILD_PQ0`.
- Raw-vector rebuild K10 probe results:
  - `L_search=10, beam=16`: `recall@10=0.832300`.
  - `L_search=4096, beam=16`: `recall@10=0.998800`.
  - `L_search=8192, beam=16`: `recall@10=0.998900`.
  - `L_search=16384, beam=16`: `recall@10=0.998900`.
- Decision:
  - The fresh raw-vector rebuild does not satisfy the ImageNet K10 high endpoint (`0.999400`), so it is not eligible for promotion.
  - The best raw-vector ImageNet candidate remains the earlier `diskann_disk_imagenet1k_R13_L100_PQ0`, which reached `0.999100` at K10 and covered K50, but still misses the K10 high endpoint by `0.000300`.
  - ImageNet DiskANN rows remain unpromoted; task 7.12 remains incomplete.
- Disk/process status after correction:
  - No active DiskANN build/search process remained.
  - `/home/zcq/VDB` available: about `1006G`, use `70%`.

## 2026-06-02T13:55:55+0800 ImageNet Raw-Only Follow-Up

- Reconfirmed the raw-vector-only constraint after the correction:
  - ImageNet DiskANN manifest scan found `0` remaining index directories with `pq_disk_bytes > 0`.
  - Removed the generated PQ/reorder ImageNet DiskANN probe CSVs from the isolated probe directory so they cannot be mistaken for valid candidates:
    - `diskann_imagenet_r64_l100_pq32_probe_20260602.csv`.
    - `diskann_imagenet_r64_l100_pq8_reorder_no_use_probe_20260602.csv`.
    - `diskann_imagenet_r64_l100_pq8_reorder_reuse_probe_20260602.csv`.
- Ran an additional raw-vector candidate probe:
  - Index id: `diskann_disk_imagenet1k_R14_L49_PQ0`.
  - Build/search used `--PQ_disk_bytes 0`.
  - `topk=50`, `L_search=50`, `beam=1`: `recall@50=0.889000`.
  - Decision: rejected because the low endpoint is still above the IVF+RaBitQ low endpoint `0.888340`; no high-end probe was promoted for this index.
- Reused the accepted-high raw index to test whether a smaller beam could lower the K50 low point:
  - Index id: `diskann_disk_imagenet1k_R40_L100_PQ0`.
  - `topk=50`, `L_search=50`, `beam=0` returned `search_failed=-11`, so this setting is invalid and not considered.
- Reused the closest raw index for a diagnostic candidate-cover probe:
  - Index id: `diskann_disk_imagenet1k_R13_L100_PQ0`.
  - `topk=100`, `L_search=4096`, `beam=16`: direct `recall@10=0.999100`, `recall@50=0.999280`, `recall@100=0.999160`.
  - The top-100 candidate set covered GT@10 at `0.999600`, but direct DiskANN ordering still reports `0.999100` at K10. Offline exact reranking was not used because it exposed a metric/order mismatch with the existing GT and would not be comparable to the C++ DiskANN rows.
- Current raw-only decision:
  - No ImageNet DiskANN raw-vector index tested so far satisfies the single-index coverage gate for both K10 and K50.
  - Best valid raw-only candidate remains `diskann_disk_imagenet1k_R13_L100_PQ0`: K10 high misses by `0.000300`, K50 covers both endpoints.
  - ImageNet DiskANN remains unpromoted; task 7.12 remains incomplete and the main result tables are unchanged for ImageNet DiskANN.

## 2026-06-02T15:37:36+0800 ImageNet DiskANN Relaxed-Cap Promotion

- Operator update: ImageNet DiskANN no longer needs to chase the IVF+RaBitQ high endpoint `0.999400`; the accepted high-recall endpoint only needs to reach `0.995`.
- Updated OpenSpec acceptance wording and the ICDE report generator to evaluate high coverage against `min(IVF+RaBitQ high, 0.995)` when an operator cap is requested.
- Accepted raw-vector ImageNet DiskANN index:
  - Index id: `diskann_disk_imagenet1k_R13_L100_PQ0`.
  - Manifest identity: `dfe8c8ac31c3`.
  - Build/search storage: `pq_disk_bytes=0`, `ssd_vector_storage=raw`.
  - Build command recorded `--PQ_disk_bytes 0`.
- Promoted ImageNet DiskANN rows to `/home/zcq/VDB/baselines/formal-study/outputs/icde_baselines/diskann_icde_baselines.csv`:
  - Backup: `backups/diskann_icde_baselines_before_imagenet_r13_l100_relaxed_20260602_152724.csv`.
  - Summary/selected/report backups before promotion: `backups/icde_baseline_summary_before_imagenet_diskann_promote_20260602_152724.csv`, `backups/icde_baseline_selected_before_imagenet_diskann_promote_20260602_152724.csv`, `backups/icde_baseline_report_before_imagenet_diskann_promote_20260602_152724.md`.
  - Promoted rows: `24` valid ImageNet DiskANN rows.
  - Backends: `flatstor,lance`.
  - Top-k: `10,50`.
  - Unique ImageNet DiskANN index id: `diskann_disk_imagenet1k_R13_L100_PQ0`.
- Promoted ImageNet DiskANN recall coverage over FlatStor rows:
  - `topk=10`: `0.709500` to `0.999100`; low endpoint covers IVF+RaBitQ low `0.925500`, high endpoint covers capped target `0.995000`.
  - `topk=50`: `0.881320` to `0.999280`; low endpoint covers IVF+RaBitQ low `0.888340`, high endpoint covers capped target `0.995000`.
- Aggregated summary after promotion:
  - Command: `run_icde_baseline_suite.py --stage aggregate --datasets imagenet1k,voxceleb2_ecapa_150k`.
  - Summary rows: `579` total, `148` for ImageNet/Vox.
  - Selected rows: `180` total, `72` for ImageNet/Vox.
  - Completed dataset/system/backend/topk groups: `60/60`.
  - ImageNet/Vox systems: `DiskANN`, `IVF+PQ`, `IVF+RQ`.
  - ImageNet/Vox backends: `flatstor,lance`; Parquet rows: `0`.
  - ImageNet/Vox top-k tiers: `10,50`; topk=20 rows: `0`.
  - Expansion DiskANN single-index validation passed:
    - ImageNet: `diskann_disk_imagenet1k_R13_L100_PQ0`.
    - VoxCeleb2: `diskann_disk_voxceleb2_ecapa_150k_R13_L100_PQ0`.
  - Report updated at `/home/zcq/VDB/baselines/formal-study/outputs/icde_baselines/icde_baseline_report.md` with required backend scope, no-Parquet/no-Recall@20 invariants, accepted index metadata, search grids, IVF+RaBitQ ranges, DiskANN ranges, and high-cap coverage decisions.
- Final disk/process status:
  - No active ImageNet DiskANN build/search process remained.
  - `/home/zcq/VDB` available: about `964G`, use `71%`.
  - ICDE baseline output directory: `329M`.
  - Accepted ImageNet DiskANN index size: `11G`.
  - Accepted VoxCeleb2 DiskANN index size: `406M`.
