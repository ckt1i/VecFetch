## Why

The ICDE experiment plan now uses `Recall@10` and `Recall@100` as the main top-k tiers, but the current formal baseline outputs and ICDE suite still aggregate only `topk=10/50`. We need a controlled rerun path that adds `Recall@100` results across the existing formal baseline datasets and baseline methods without rebuilding indexes or changing the one-index-per-baseline state that is now on disk.

## What Changes

- Extend the ICDE baseline runner and aggregation path to support `topk=100` while preserving existing `topk=10` results.
- Keep the formal main-summary surface to `topk ∈ {10,100}` for this change; old `topk=50` outputs may remain on disk but SHALL NOT enter the refreshed ICDE main summary or selected table.
- Add `recall@100` to the shared IVF/E2E/DiskANN metric schemas and propagate it into `icde_baseline_summary.csv`, `icde_baseline_selected.csv`, and the Markdown report.
- Rerun `topk=100` for the five currently runnable ICDE formal baseline datasets:
  - `coco_100k`
  - `msmarco_passage`
  - `amazon_esci`
  - `imagenet1k`
  - `voxceleb2_ecapa_150k`
- Rerun all six baseline combinations for `topk=100`:
  - `IVF+PQ+FlatStor`
  - `IVF+PQ+Lance`
  - `IVF+RQ+FlatStor`
  - `IVF+RQ+Lance`
  - `DiskANN+FlatStor`
  - `DiskANN+Lance`
- Reuse existing FAISS/IVF, RaBitQ, payload backend, and DiskANN index artifacts whenever possible.
- Enforce a no-autonomous-build policy for DiskANN: the runner SHALL only use existing DiskANN index directories and SHALL fail fast if a required index is missing or its manifest does not match.
- Enforce one retained DiskANN index identity per dataset in the refreshed DiskANN baseline results. For `amazon_esci`, the refreshed `topk=10/100` result surface SHALL use the existing `diskann_disk_amazon_esci_R64_L100_PQ0` index and SHALL exclude the older `R32_L50_PQ0` rows from the refreshed main summary.
- Do not require `Recall@100` to reach `0.95`; aggregate best-effort rows and threshold-matched rows honestly according to achieved recall.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `formal-baseline-execution`: update ICDE baseline execution to support the `topk=100` rerun using existing indexes only, with no autonomous DiskANN builds.
- `formal-baseline-result-tracking`: update aggregation and reporting so refreshed ICDE main outputs use `topk=10/100`, include `recall@100`, preserve old `topk=50` artifacts outside the main result surface, and record best-effort selections when `0.95` is not reached.

## Impact

- Affects `/home/zcq/VDB/baselines/formal-study/scripts/run_icde_baseline_suite.py`.
- Affects shared metric/evaluation utilities under `/home/zcq/VDB/baselines/formal-study/scripts/_shared/`.
- Affects DiskANN C++ runner reporting under `/home/zcq/VDB/baselines/vector_search/run_diskann_cpp.py`.
- Writes additional `topk=100` run outputs under `/home/zcq/VDB/baselines/formal-study/outputs/`.
- Updates ICDE baseline summary, selected, and report files under `/home/zcq/VDB/baselines/formal-study/outputs/icde_baselines/`, with backups before replacement.
- Does not modify or delete raw datasets, payload backends, IVF/RaBitQ caches, existing DiskANN index directories, or prior `topk=50` result artifacts. Unused existing DiskANN directories may remain on disk unless a separate cleanup request is made.
