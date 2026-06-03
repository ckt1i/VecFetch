## Why

ImageNet-1K and VoxCeleb2 embeddings have now been materialized locally, but the formal baseline pipeline still only treats COCO100K, MS MARCO, and Amazon ESCI as runnable ICDE datasets. This change makes the new image and audio datasets runnable through the same FlatStor/Lance baseline path without paying for unnecessary Parquet exports or duplicate DiskANN graph builds.

## What Changes

- Add formal-baseline support for `imagenet1k` and `voxceleb2_ecapa_150k`, including cleaned payload metadata, split manifests, exact top-100 ground truth, and FlatStor/Lance payload exports.
- Do not build, require, run, or aggregate the Parquet backend for these two datasets.
- Extend the ICDE baseline suite so it can run only selected datasets instead of forcing the old full dataset matrix.
- Run the six required baseline combinations for the new datasets:
  - `IVF+PQ+FlatStor`
  - `IVF+PQ+Lance`
  - `IVF+RQ+FlatStor`
  - `IVF+RQ+Lance`
  - `DiskANN+FlatStor`
  - `DiskANN+Lance`
- Keep the ICDE metric surface limited to `Recall@10` and `Recall@50`; do not reintroduce `Recall@20`.
- Reuse IVF/RaBitQ index caches and shared vector-search outputs across FlatStor and Lance wherever parameters match.
- For each dataset, use only one final DiskANN build configuration in the promoted baseline table; the selected graph index must have an `L_search`/beam sweep whose recall range covers the corresponding IVF+RaBitQ recall range:
  - the lowest valid DiskANN recall point is at or below the lowest valid IVF+RaBitQ point for the same dataset/top-k where possible;
  - the highest valid DiskANN recall point is at or above the highest valid IVF+RaBitQ point for the same dataset/top-k, unless an operator cap is explicitly requested; the current ImageNet promotion uses a high-end cap of `0.995`.
- Keep DiskANN tuning/probe rows isolated until the single-index coverage check passes, then promote only the accepted index rows into the main ICDE baseline summary.
- Preserve disk-space safety: avoid unpacking ImageNet/Vox raw archives into duplicate full file trees, and do not delete any user-provided raw downloads or existing experiment results.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `formal-baseline-data-prep`: add ImageNet-1K and VoxCeleb2 formal dataset contracts, with FlatStor/Lance-only payload readiness for this change.
- `formal-baseline-execution`: add selected-dataset ICDE baseline execution for ImageNet/Vox and require single-index DiskANN recall-range coverage against IVF+RaBitQ.
- `formal-baseline-result-tracking`: add isolated probe tracking and promotion rules for new dataset baselines, including explicit no-Parquet and no-Recall@20 aggregation checks.

## Impact

- Affects dataset preparation scripts under `/home/zcq/VDB/baselines/formal-study/scripts/prepare_datasets/`, `build_groundtruth/`, and `export_payload_backends/`.
- Affects shared dataset loading and readiness checks under `/home/zcq/VDB/baselines/formal-study/scripts/_shared/`.
- Affects ICDE baseline orchestration, aggregation, and reporting under `/home/zcq/VDB/baselines/formal-study/scripts/run_icde_baseline_suite.py`.
- Affects DiskANN C++ runner support under `/home/zcq/VDB/baselines/vector_search/run_diskann_cpp.py`.
- Writes new formatted dataset assets under `/home/zcq/VDB/baselines/data/formal_baselines/{imagenet1k,voxceleb2_ecapa_150k}/`.
- Writes experiment outputs under `/home/zcq/VDB/baselines/formal-study/outputs/`, with probe outputs kept separate until explicitly promoted.
- Uses existing raw downloads and embeddings under `/home/zcq/VDB/data/raw_downloads/` and `/home/zcq/VDB/data/formal_baselines/`.
