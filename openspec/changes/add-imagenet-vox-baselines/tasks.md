## 1. Scope And Safety Checks

- [x] 1.1 Record current disk usage for `/home/zcq/VDB`, `/home/zcq/VDB/data`, `/home/zcq/VDB/baselines/data`, and `/home/zcq/VDB/baselines/formal-study/outputs`.
- [x] 1.2 Confirm no active baseline, embedding, FAISS, or DiskANN process is already running before starting implementation runs.
- [x] 1.3 Record the current shapes and metadata for `imagenet1k` and `voxceleb2_ecapa_150k` raw embeddings.
- [x] 1.4 Add an implementation note that raw downloads and prior result directories must not be deleted during this change.

## 2. Dataset Registry And Defaults

- [x] 2.1 Add `imagenet1k` to the formal dataset registry with CLIP image embedding metadata, cosine metric, and FlatStor/Lance storage backends.
- [x] 2.2 Add `voxceleb2_ecapa_150k` to the formal dataset registry with ECAPA embedding metadata, cosine metric, and FlatStor/Lance storage backends.
- [x] 2.3 Add dataset-specific ICDE defaults for row count, dimension, `nlist`, `nprobe` sweep, and DiskANN probe grid.
- [x] 2.4 Add a dataset-selection option to the ICDE runner so new dataset runs do not force old dataset reruns.
- [x] 2.5 Add a dataset-specific required-backends contract so ImageNet/Vox readiness requires only `flatstor,lance`.

## 3. Formal Dataset Materialization

- [x] 3.1 Implement or extend a preparation script that creates `/home/zcq/VDB/baselines/data/formal_baselines/imagenet1k/` without unpacking ImageNet archives into a duplicate full file tree.
- [x] 3.2 Stream ImageNet train JPEG payloads from the official tar archives into a row-aligned cleaned payload source for base records.
- [x] 3.3 Write ImageNet split metadata with fixed query ids and base row count under `splits/split_v1.json`.
- [x] 3.4 Implement or extend a preparation script that creates `/home/zcq/VDB/baselines/data/formal_baselines/voxceleb2_ecapa_150k/` without duplicating WDS archive contents.
- [x] 3.5 Stream VoxCeleb2 base audio payloads and speaker metadata from WDS tar shards into a row-aligned cleaned payload source.
- [x] 3.6 Write VoxCeleb2 split metadata with fixed query ids and base row count under `splits/split_v1.json`.
- [x] 3.7 Validate that every new formatted dataset has payload row ids in `[0, base_row_count)` and exactly one payload per base vector.

## 4. Payload Backend Exports

- [x] 4.1 Extend payload export code to support ImageNet FlatStor from the new cleaned payload source.
- [x] 4.2 Extend payload export code to support ImageNet Lance from the new cleaned payload source.
- [x] 4.3 Extend payload export code to support VoxCeleb2 FlatStor from the new cleaned payload source.
- [x] 4.4 Extend payload export code to support VoxCeleb2 Lance from the new cleaned payload source.
- [x] 4.5 Ensure `payload_parquet/default` is neither generated nor required for ImageNet/Vox in this change.
- [x] 4.6 Validate that FlatStor and Lance row counts match base embedding row counts for both datasets.

## 5. Exact Ground Truth And Dataset Loading

- [x] 5.1 Implement chunked exact top-100 ground-truth generation for ImageNet using normalized cosine or inner-product scoring.
- [x] 5.2 Implement chunked exact top-100 ground-truth generation for VoxCeleb2 using normalized cosine or inner-product scoring.
- [x] 5.3 Write `gt_top10.npy`, `gt_top20.npy`, and `gt_top100.npy` for both datasets while keeping ICDE aggregation limited to K=10/50.
- [x] 5.4 Extend `_shared.datasets.load_dataset_bundle()` to load `imagenet1k` and `voxceleb2_ecapa_150k`.
- [x] 5.5 Extend dataset readiness summaries so ImageNet/Vox pass phase-0 readiness with FlatStor/Lance only.
- [x] 5.6 Run readiness validation for both datasets and save the generated sanity manifests.

## 6. IVF Baseline Execution

- [x] 6.1 Run a VoxCeleb2 IVF+PQ smoke point for FlatStor and Lance at `topk=10`.
- [x] 6.2 Run a VoxCeleb2 IVF+RaBitQ smoke point for FlatStor and Lance at `topk=10`.
- [x] 6.3 Run the full VoxCeleb2 IVF+PQ and IVF+RaBitQ sweeps for `topk=10` and `topk=50`.
- [x] 6.4 Run an ImageNet IVF+PQ smoke point for FlatStor and Lance at `topk=10`.
- [x] 6.5 Run an ImageNet IVF+RaBitQ smoke point for FlatStor and Lance at `topk=10`.
- [x] 6.6 Run the full ImageNet IVF+PQ and IVF+RaBitQ sweeps for `topk=10` and `topk=50`.
- [x] 6.7 Verify that matching FlatStor and Lance IVF rows reuse the same ANN search parameters and vector-search outputs.
- [x] 6.8 Compute IVF+RaBitQ recall min/max ranges per dataset and top-k tier for DiskANN coverage gating.

## 7. DiskANN Runner And Coverage Gate

- [x] 7.1 Extend `run_diskann_cpp.py` dataset specs and CLI choices to support `imagenet1k` and `voxceleb2_ecapa_150k`.
- [x] 7.2 Ensure DiskANN query loading for the new datasets evaluates `topk=10` and `topk=50` against `gt_top100.npy`.
- [x] 7.3 Ensure DiskANN FlatStor and Lance runs use the same graph/index identity and only differ in payload fetch backend.
- [x] 7.4 Add or script an isolated DiskANN probe output path for ImageNet/Vox candidate index testing.
- [x] 7.5 Run a VoxCeleb2 DiskANN candidate index probe and evaluate recall coverage against IVF+RaBitQ.
- [x] 7.6 If VoxCeleb2 DiskANN low-recall coverage fails, expand toward lower `L_search` or beam settings before building a new index.
- [x] 7.7 If VoxCeleb2 DiskANN high-recall coverage fails after search-grid expansion, build a stronger graph index and re-run the probe.
- [x] 7.8 Promote exactly one accepted VoxCeleb2 DiskANN index into full FlatStor and Lance sweeps for K=10/50.
- [x] 7.9 Run an ImageNet DiskANN candidate index probe and evaluate recall coverage against IVF+RaBitQ.
- [x] 7.10 If ImageNet DiskANN low-recall coverage fails, expand toward lower `L_search` or beam settings before building a new index.
- [x] 7.11 If ImageNet DiskANN high-recall coverage fails after search-grid expansion, build a stronger graph index and re-run the probe.
- [x] 7.12 Promote exactly one accepted ImageNet DiskANN index into full FlatStor and Lance sweeps for K=10/50.

## 8. Aggregation, Promotion, And Validation

- [x] 8.1 Extend ICDE aggregation to include selected ImageNet/Vox rows while excluding Parquet and Recall@20 rows.
- [x] 8.2 Keep DiskANN probe rows out of the main summary until the single-index coverage check passes.
- [x] 8.3 Back up any existing ICDE summary CSV before promoting ImageNet/Vox rows.
- [x] 8.4 Promote only accepted rows for the corresponding dataset/system/backend/top-k scope.
- [x] 8.5 Validate that each promoted `dataset × DiskANN` group has exactly one non-empty index id.
- [x] 8.6 Validate that ImageNet/Vox summaries contain only `flatstor,lance` backends.
- [x] 8.7 Validate that ImageNet/Vox summaries contain only `topk=10` and `topk=50`.
- [x] 8.8 Generate or update the ICDE baseline report with dataset readiness, build/reuse status, DiskANN coverage metadata, and selected operating points.
- [x] 8.9 Record final disk usage and output sizes after the baseline runs complete.
