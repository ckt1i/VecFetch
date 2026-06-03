# Implementation Notes

## Scope

- Refreshed ICDE baseline surface for `coco_100k`, `voxceleb2_ecapa_150k`, `amazon_esci`, `imagenet1k`, and `msmarco_passage`.
- Active top-k tiers are `10,100`; stale `topk=20` and `topk=50` rows are excluded from refreshed summary, selected, and report outputs.
- Payload backends are `flatstor` and `lance`; Parquet is not included.
- `Recall@100` selection targets are `0.80`, `0.90`, `0.95`, and `0.995`. The `0.995` point is the highest target and is not a hard pass requirement.

## Code Changes

- Added `recall@100` to shared vector-search/E2E metric fields and recall metric computation.
- Added `recall@100` to DiskANN CSV output.
- Added `--topks` support to `run_icde_baseline_suite.py`.
- Added accepted existing DiskANN index validation and a no-build guard through `--existing-index-only`.
- Restricted refreshed aggregation to accepted raw-vector DiskANN indexes with `pq_disk_bytes=0`.
- Updated aggregation/reporting to include `0.995` as the highest `Recall@100` selection target.

## Accepted DiskANN Indexes

- `coco_100k`: `/home/zcq/VDB/baselines/data/diskann_disk_coco_100k_R64_L100_PQ0`
- `msmarco_passage`: `/home/zcq/VDB/baselines/data/diskann_disk_msmarco_passage_R32_L50_PQ0`
- `amazon_esci`: `/home/zcq/VDB/baselines/data/diskann_disk_amazon_esci_R64_L100_PQ0`
- `imagenet1k`: `/home/zcq/VDB/baselines/data/diskann_disk_imagenet1k_R13_L100_PQ0`
- `voxceleb2_ecapa_150k`: `/home/zcq/VDB/baselines/data/diskann_disk_voxceleb2_ecapa_150k_R13_L100_PQ0`

All accepted manifests were checked for dataset identity, build parameters, `pq_disk_bytes=0`, and raw vector storage.

## Execution

- CPU/disk preflight checks were run before the major experiment stages; `/home/zcq/VDB` remained at about `1.1T` free.
- IVF runs were executed in order: `coco_100k,voxceleb2_ecapa_150k`, then `amazon_esci,imagenet1k`, then `msmarco_passage`.
- DiskANN runs were executed in the same order with explicit `--index-dir`, `--pq-disk-bytes 0`, and `--existing-index-only`.
- DiskANN dry-run showed no command invoking `build_disk_index`.

## Outputs

- Summary: `/home/zcq/VDB/baselines/formal-study/outputs/icde_baselines/icde_baseline_summary.csv`
- Selected: `/home/zcq/VDB/baselines/formal-study/outputs/icde_baselines/icde_baseline_selected.csv`
- Report: `/home/zcq/VDB/baselines/formal-study/outputs/icde_baselines/icde_baseline_report.md`
- Output directory size after aggregation: `489M`.
- Latest aggregate backups:
  - `/home/zcq/VDB/baselines/formal-study/outputs/icde_baselines/backups/icde_baseline_summary_before_aggregate_20260603_041422.csv`
  - `/home/zcq/VDB/baselines/formal-study/outputs/icde_baselines/backups/icde_baseline_selected_before_aggregate_20260603_041422.csv`
  - `/home/zcq/VDB/baselines/formal-study/outputs/icde_baselines/backups/icde_baseline_report_before_aggregate_20260603_041422.md`

## Validation

- `py_compile` passed for the updated suite runner.
- Summary rows: `588`.
- Selected rows: `207`.
- Summary and selected top-k tiers: `10,100` only.
- Completed refreshed `topk=100` groups: `30/30`.
- Every `dataset x system x backend x topk=100` group has at least one valid row.
- DiskANN `topk=100` rows: `180`, all `index_status=reused`, all `pq_disk_bytes=0`, and each dataset references exactly one accepted index.
- `Recall@100` selected targets: `0.80`, `0.90`, `0.95`, `0.995`.
- `Recall@100` selected rows: `120`; matched `86`, best-effort `34`.

DiskANN raw sweep maxima at `topk=100`:

- `amazon_esci`: `0.981120`
- `coco_100k`: `0.999260`
- `imagenet1k`: `0.996190`
- `msmarco_passage`: `0.976340`
- `voxceleb2_ecapa_150k`: `0.995450`

Raw sweep rows above `0.995` are retained for traceability, but `0.995` is the highest selected target.

## Final System State

- `/home/zcq/VDB`: `3.5T` total, `2.3T` used, `1.1T` available, `69%` used.
- No active processes matched `run_icde_baseline_suite.py`, `run_diskann_cpp.py`, `build_disk_index`, `search_disk_index`, `faiss_ivfpq`, or `ivf_rabitq`.
