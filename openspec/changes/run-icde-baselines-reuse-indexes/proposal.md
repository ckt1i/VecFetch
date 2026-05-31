## Why

The ICDE experiment plan now requires explicit payload-store baselines for `Recall@10` and `Recall@50`, but the existing formal-study execution is still organized around a FlatStor-first main suite, a top-10 backend extension, and a DiskANN C++ runner that only covers part of the required matrix. We need a scoped execution change that produces usable COCO100K, MS MARCO, and Amazon ESCI baseline results quickly while extending DiskANN without duplicating large indexes unnecessarily.

## What Changes

- Run the first baseline wave on `coco_100k`, `msmarco_passage`, and `amazon_esci`.
- Produce the four IVF payload-store combinations first:
  - `IVF+PQ+FlatStor`
  - `IVF+PQ+Lance`
  - `IVF+RQ+FlatStor`
  - `IVF+RQ+Lance`
- Use only `topk=10` and `topk=50` for the ICDE baseline run; do not schedule `topk=20` or `topk=100` for this change.
- Reuse existing FAISS and RaBitQ/RQ index caches wherever parameters match the required run matrix.
- Extend the DiskANN C++ baseline path after the IVF wave so it can:
  - use `gt_top100.npy` and evaluate both `Recall@10` and `Recall@50`;
  - read payloads through both FlatStor and Lance;
  - add Amazon ESCI dataset support;
  - reuse existing COCO100K and MS MARCO DiskANN indexes by manifest identity before building new ones.
- Run DiskANN baselines after the runner extension:
  - `DiskANN+FlatStor`
  - `DiskANN+Lance`
- Emit ICDE-ready baseline summaries with dataset, system, backend, top-k, parameters, recall, latency, read count, bytes read, index identity, and reuse/build status.
- Update the experiment tracker to match the current ICDE plan and remove stale `Recall@20` / `topk=100` baseline rows from the active path.

## Capabilities

### New Capabilities

None.

### Modified Capabilities

- `formal-baseline-execution`: update the formal baseline execution contract for the ICDE six-combination baseline matrix, staged IVF-before-DiskANN execution, `Recall@10`/`Recall@50` only, and index reuse requirements.

## Impact

- Affects formal baseline scripts under `/home/zcq/VDB/baselines/formal-study/scripts/`, especially coupled E2E runners, aggregation, controls, and tracker usage.
- Affects DiskANN C++ baseline code under `/home/zcq/VDB/baselines/vector_search/run_diskann_cpp.py`.
- Affects formal-study outputs under `/home/zcq/VDB/baselines/formal-study/outputs/` and selected promoted result CSVs under `/home/zcq/VDB/baselines/results/`.
- Reuses existing data under `/home/zcq/VDB/baselines/data/formal_baselines/{coco_100k,msmarco_passage,amazon_esci}` and existing index caches where possible.
- Depends on `/home/zcq/anaconda3/envs/labnew/bin/python` for FAISS and LanceDB availability.
