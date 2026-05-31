## 1. Scope And Tracker Alignment

- [ ] 1.1 Update the paper-side ICDE experiment tracker so the active baseline rows only include `topk=10` and `topk=50`.
- [ ] 1.2 Replace stale baseline tracker rows with the six required combinations: IVF+PQ/IVF+RQ/DiskANN crossed with FlatStor/Lance.
- [ ] 1.3 Record the execution environment requirement that formal baseline runs use `/home/zcq/anaconda3/envs/labnew/bin/python`.
- [ ] 1.4 Confirm COCO100K, MS MARCO, and Amazon ESCI phase-0 readiness from existing sanity manifests before scheduling runs.

## 2. IVF Baseline Execution

- [ ] 2.1 Define an ICDE run list for `coco_100k`, `msmarco_passage`, and `amazon_esci` with `topk ∈ {10,50}`.
- [ ] 2.2 Schedule IVF+PQ+FlatStor and IVF+PQ+Lance runs with fixed `nlist`, `nprobe` sweep, and `candidate_budget=topk*20`.
- [ ] 2.3 Schedule IVF+RQ/RaBitQ+FlatStor and IVF+RQ/RaBitQ+Lance runs with fixed `nlist`, `nprobe` sweep, `total_bits`, and `candidate_budget=topk*20`.
- [ ] 2.4 Verify that matching FlatStor and Lance rows reuse the same search index identity and search parameters.
- [ ] 2.5 Aggregate the IVF four-combination outputs into an ICDE baseline summary with recall, avg/p50/p95/p99, bytes read, fetch count, backend, and selected parameter fields.

## 3. DiskANN Runner Extension

- [ ] 3.1 Extend the DiskANN C++ runner dataset specs to include Amazon ESCI base/query embeddings, `gt_top100.npy`, and payload roots.
- [ ] 3.2 Change DiskANN recall loading so `topk=10` and `topk=50` both evaluate against `gt_top100.npy`.
- [ ] 3.3 Add a `--backend flatstor|lance` option to the DiskANN C++ runner and route payload fetch through the shared payload backend adapter.
- [ ] 3.4 Preserve C++ CLI search as the only official DiskANN search path and keep duplicate/incomplete result rejection.
- [ ] 3.5 Record DiskANN run metadata including dataset, backend, top-k, index directory, manifest identity, search parameters, and whether the index was reused or built.

## 4. DiskANN Index Reuse And Builds

- [ ] 4.1 Reuse existing COCO100K DiskANN indexes when their manifests match the requested `R`, `L_build`, metric, row count, dimension, and `pq_disk_bytes`.
- [ ] 4.2 Reuse existing MS MARCO DiskANN PQ0 indexes for compatible `topk=10` and `topk=50` sweeps before considering new builds.
- [ ] 4.3 Build the first Amazon ESCI DiskANN index only after a manifest lookup confirms no compatible index exists.
- [ ] 4.4 Add smoke runs for each DiskANN dataset/backend pair before launching full sweeps.
- [ ] 4.5 Run DiskANN+FlatStor and DiskANN+Lance sweeps for COCO100K, MS MARCO, and Amazon ESCI at `topk=10` and `topk=50`.

## 5. ICDE Aggregation And Validation

- [ ] 5.1 Add or update an ICDE aggregation path that selects fastest valid points at the planned recall thresholds for `Recall@10` and `Recall@50`.
- [ ] 5.2 Ensure aggregation excludes invalid DiskANN rows and stale `topk=20` or `topk=100` rows.
- [ ] 5.3 Emit a compact ICDE baseline CSV and a human-readable run report listing completed, reused, built, blocked, and best-effort rows.
- [ ] 5.4 Compare the new summary against existing COCO/MS MARCO DiskANN PQ0 results to confirm no recall-definition regression.
- [ ] 5.5 Run targeted smoke/validation commands for IVF E2E, Lance payload fetch, DiskANN C++ parsing, and aggregation.
