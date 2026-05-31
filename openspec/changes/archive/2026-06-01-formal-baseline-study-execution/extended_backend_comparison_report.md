# Extended Backend Comparison Report (8.14)

## Status Snapshot

- Operating-point selection and replay for all target datasets are complete under `topk=10`:
  - `coco_100k`: `faiss_ivfpq_refine`, `ivf_rabitq_rerank`
  - `msmarco_passage`: `faiss_ivfpq_refine`, `ivf_rabitq_rerank`
  - `deep8m_synth`: `faiss_ivfpq_refine`, `ivf_rabitq_rerank` (256B/4KB/64KB variants)
  - `amazon_esci`: `faiss_ivfpq_refine`, `ivf_rabitq_rerank`
- Backends replayed: `lance`, `parquet` (primary backend `flatstor` is used via main suite and retained for manifest/reference).
- Selection states in `/home/zcq/VDB/baselines/formal-study/outputs/extended_backend/selected_operating_points.csv`:
  - `matched`: 27
  - `best-effort`: 9
  - `unreached`: 0

## Generated Artifacts

- `/home/zcq/VDB/baselines/formal-study/outputs/summaries/extended_backend_summary.csv`
  - full extended backend E2E rows matched to `selected_operating_points.csv`.
- `/home/zcq/VDB/baselines/formal-study/outputs/summaries/extended_backend_summary_plot_ready.csv`
  - pivoted table by `(dataset, system, variant, target_recall)` with `flatstor/lance/parquet` `e2e_ms` columns.

## Key Observations

- `ivf_rabitq_rerank` consistently keeps lower `e2e_ms` than `faiss_ivfpq_refine` at similar target recall levels, while `deep8m_synth` remains the lowest-latency family across all backends.
- For most dataset/system combinations, `parquet` and `lance` are the expected non-primary replay backends; no `selection_status = unreached` entries remain.
- Backend ranking is stable by system class and target recall, and payload/backend differences are visible mainly in the `e2e_ms` gap between `lance` and the other backends.

## Notes

- `tasks.md` has been updated to mark 8.12/8.13/8.14 completed.
- Figure rendering from the plot-ready CSV was not executed in this runtime due missing plotting dependency in the shared Python environment; generated CSV is ready for downstream plotting.
