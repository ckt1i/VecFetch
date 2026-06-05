## Context

The current ICDE plan requires a six-combination baseline matrix across search family and payload store:

```
                    FlatStor       Lance
IVF+PQ              required       required
IVF+RQ/RaBitQ       required       required
DiskANN             required       required
```

The available data assets are strong enough to begin with `coco_100k`, `msmarco_passage`, and `amazon_esci`: all three have base/query embeddings, `gt_top100.npy`, FlatStor payloads, and Lance payloads. The formal-study scripts already support IVF+PQ and IVF+RQ/RaBitQ coupled E2E runs with `--backend flatstor|lance` when run with the `labnew` Python environment.

DiskANN is the uneven part. The existing C++ runner is the correct official path for COCO100K/MS MARCO `DiskANN+FlatStor` because the Python binding previously produced duplicate IDs, but the runner currently needs extension for `topk=50`, Lance payload fetch, and Amazon ESCI. There is already a related change, `diskann-pareto-coco-msmarco`, that produced a COCO/MS MARCO `DiskANN+FlatStor` `Recall@10` PQ0 baseline and index manifests. This change should reuse that work rather than rebuilding the same indexes.

## Goals / Non-Goals

**Goals:**

- Produce ICDE baseline results for COCO100K, MS MARCO, and Amazon ESCI.
- Run the IVF four-combination matrix first so the paper has usable baseline rows before DiskANN work finishes.
- Extend DiskANN C++ evaluation to cover `topk=10`, `topk=50`, FlatStor, Lance, and Amazon ESCI.
- Reuse existing FAISS, RaBitQ/RQ, and DiskANN indexes whenever the dataset and build/search identity matches the requested run.
- Keep result outputs auditable by recording parameter identity, payload backend, index identity, and whether an index was reused or built.
- Keep `Recall@20` and `topk=100` out of this ICDE baseline execution path.

**Non-Goals:**

- Do not run BoundFetch/VecFetch main-method experiments in this change.
- Do not run LAION, ImageNet, VoxCeleb2, Deep8M, or ActivityNet baselines in this change.
- Do not introduce Parquet into the ICDE main baseline matrix.
- Do not use `diskannpy` as an official DiskANN result path.
- Do not rebuild existing indexes solely to refresh timestamps or directory layout.
- Do not change encoder, embedding, or ground-truth generation logic unless a blocking mismatch is discovered.

## Decisions

1. Run IVF baselines before DiskANN.

   The IVF+PQ and IVF+RQ/RaBitQ coupled E2E scripts already support FlatStor and Lance payload fetch through shared backend adapters. Running these first gives immediate COCO100K/MS MARCO/ESCI comparison data and isolates DiskANN runner work from the rest of the baseline matrix.

2. Treat `topk=10` and `topk=50` as the only active ICDE top-k tiers.

   The current formal-study controls include `topk=100`, and older tracker rows include `topk=20`. This change should bypass those stale controls and explicitly schedule only `10` and `50` for the ICDE baseline run.

3. Reuse index caches by identity before building.

   FAISS and RaBitQ/RQ runners should use their existing cache paths for matching dataset, `nlist`, quantizer parameters, and metric. DiskANN should identify reuse through manifest fields including dataset, metric, row count, dimension, `R`, `L_build`, `pq_disk_bytes`, and index prefix. Existing COCO100K/MS MARCO PQ0 DiskANN indexes should be reused for compatible runs.

4. Keep search parameters shared across payload stores.

   FlatStor and Lance runs for the same search family must use the same ANN/search parameters. Payload backend changes should affect only payload fetch timing, read count, bytes read, and backend-specific overhead, not candidate generation.

5. Extend DiskANN runner around the C++ CLI, not the Python binding.

   The C++ CLI remains the authoritative DiskANN search path. The runner should load `gt_top100.npy`, compute overlap recall for the requested `topk`, support a backend parameter for FlatStor/Lance payload fetch, and add `amazon_esci` to the dataset spec.

6. Separate execution orchestration from final aggregation.

   Per-run outputs should stay in the existing formal-study output tree. A thin ICDE aggregation step should select the fastest valid point at matched recall thresholds and emit a compact table-ready CSV without overwriting raw run outputs.

## Risks / Trade-offs

- [DiskANN ESCI build may be expensive] -> Start with one moderate ESCI index, run smoke and coarse sweep, and build stronger/weaker variants only when recall coverage requires it.
- [Lance payload timing may include table-open/cache effects] -> Warm the backend before measured queries and report both payload timing and total E2E timing with backend identity.
- [Old formal-study outputs mix stale top-k and recall fields] -> Emit a new ICDE-specific summary from fresh run directories and mark reused legacy rows only when their metric contract matches.
- [Index reuse can hide stale/mismatched artifacts] -> Require manifest or cache identity checks before reuse and reject mismatched dimensions, metrics, row counts, or quantizer parameters.
- [Running all six combinations on three datasets is time-consuming] -> Stage execution: four IVF combinations first, then DiskANN; for DiskANN, reuse existing COCO/MS MARCO indexes and only build ESCI plus missing recall coverage.

## Migration Plan

1. Keep existing formal-study outputs and DiskANN result CSVs intact.
2. Add ICDE-specific execution controls or command wrappers that schedule only the required datasets, top-k values, systems, and payload stores.
3. Run IVF four-combination baselines and aggregate a first ICDE baseline summary.
4. Extend DiskANN runner and validate with small smoke runs.
5. Run DiskANN baselines, aggregate them into the same ICDE summary schema, and mark any unreachable thresholds explicitly.
6. Update the paper-side tracker after successful aggregation.

Rollback is simple because no existing results need to be overwritten: stop using the new ICDE summary and return to the previous formal-study outputs.

## Open Questions

- Should the first ESCI DiskANN index use `R=32,L_build=50,pq_disk_bytes=0`, matching the prior MS MARCO moderate PQ0 setup, or should it use a smaller initial graph to reduce build time?
- Should the ICDE summary include both the official RaBitQ/RQ baseline and the FAISS `faiss_ivfrq` reference, or only the official RaBitQ/RQ path named as `IVF+RQ`?
