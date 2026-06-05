## Context

The current ICDE baseline path has working results for `coco_100k`, `msmarco_passage`, and `amazon_esci`, but `imagenet1k` and `voxceleb2_ecapa_150k` are only present in the raw/formal embedding plane. They do not yet have formatted payload exports, split contracts, exact ground truth, dataset registry rows, readiness checks, or runner support.

The local disk still has enough headroom for the current ImageNet-1K and VoxCeleb2-150K baseline wave, but the implementation must avoid duplicate archive extraction and unnecessary backend materialization. This change treats Parquet as out of scope for the new baseline wave: no `payload_parquet` backend export, no Parquet E2E run, and no Parquet rows in the ICDE summaries for these datasets.

DiskANN needs a stricter promotion rule than the earlier runs. Probe rows may test multiple graph/index configurations, but the final main table must use one DiskANN index per dataset. That index's search grid must span the IVF+RaBitQ recall distribution for the same dataset and top-k tier so the DiskANN comparison is not biased by using only a high-recall or low-recall segment.

## Goals / Non-Goals

**Goals:**

- Make `imagenet1k` and `voxceleb2_ecapa_150k` runnable through the formal baseline pipeline.
- Generate or validate the formatted assets required for FlatStor and Lance baselines only.
- Add selected-dataset orchestration so ImageNet/Vox runs do not force a rerun of all older datasets.
- Run IVF+PQ, IVF+RaBitQ, and DiskANN each with FlatStor and Lance for `topk=10` and `topk=50`.
- Reuse index caches and vector-search outputs across payload backends when the search parameters match.
- Promote only one DiskANN index per dataset after a recall-coverage gate against IVF+RaBitQ passes.
- Preserve existing raw downloads, embeddings, prior results, and unrelated indexes.

**Non-Goals:**

- Do not run or aggregate the Parquet payload backend for ImageNet/Vox.
- Do not compute, schedule, or report `Recall@20`.
- Do not expand VoxCeleb2 to 1M in this change unless the 150K path is already complete and explicitly resumed later.
- Do not include encoder inference time in query latency.
- Do not delete user-provided raw archives or existing result directories to save space.

## Decisions

### Decision 1: Treat ImageNet/Vox as formal datasets with FlatStor/Lance-only backend readiness

The formatted dataset plane will still use the existing raw/derived split:

```text
/home/zcq/VDB/data/formal_baselines/<dataset>/
  embeddings/
  raw/

/home/zcq/VDB/baselines/data/formal_baselines/<dataset>/
  cleaned/
  splits/
  gt/
  payload_flatstor/default/
  payload_lance/default/
```

For this change, readiness for `imagenet1k` and `voxceleb2_ecapa_150k` must not require `payload_parquet/default`. Existing datasets may keep their prior Parquet assets, but the new datasets will not export or run that backend.

Alternative considered: keep the global readiness contract requiring FlatStor, Lance, and Parquet for every dataset. That would add avoidable storage and runtime cost for a backend the updated baseline plan does not need.

### Decision 2: Stream raw payload materialization instead of unpacking full archives

ImageNet payload export should stream JPEG bytes from official train/validation tar files and write only the base-record payload store needed for retrieval. VoxCeleb2 should stream audio bytes and metadata from WDS tar shards. The implementation may write compact metadata or cleaned manifests, but it must not extract a full duplicate file tree of all raw assets.

Alternative considered: unpack archives into per-record files and build payload stores from paths. That is simpler to inspect but risks hundreds of GB of duplicate storage for ImageNet and makes cleanup ambiguous.

### Decision 3: Generate exact top-100 once and derive top-k metrics from it

Both new datasets will use normalized embedding dot product/cosine scoring. Ground truth should be exact top-100 for the fixed query split, with `gt_top10.npy`, `gt_top20.npy`, and `gt_top100.npy` available if shared utilities still expect them. The ICDE aggregation must only consume `Recall@10` and `Recall@50`.

Alternative considered: generate only top-50. Existing loaders and sanity checks are already built around top-100 assets, so keeping top-100 avoids special cases while still keeping the reported metric surface limited to K=10/50.

### Decision 4: Add selected-dataset execution rather than cloning the suite

The ICDE suite should accept an explicit dataset list, such as `imagenet1k,voxceleb2_ecapa_150k`, and schedule only that subset. Dataset-specific defaults should be derived from row count:

- ImageNet-1K: `nlist` near `4 * sqrt(N)`, rounded to a practical power-of-two or existing FAISS-friendly value.
- VoxCeleb2-150K: smaller `nlist` appropriate for 150K rows.
- `candidate_budget=topk*20` for both IVF+PQ and IVF+RaBitQ, matching the current ICDE baseline convention.

Alternative considered: add the new datasets to the hard-coded suite immediately. That would increase accidental rerun risk and make partial execution harder to audit.

### Decision 5: Gate DiskANN promotion on one-index recall coverage

For each new dataset, DiskANN probing can build or reuse candidate graph indexes, but the final promoted ICDE rows must reference a single accepted index directory per dataset. The accepted index must satisfy coverage against IVF+RaBitQ for each top-k tier:

```text
min(DiskANN recall over accepted L_search/beam grid) <= min(IVF+RaBitQ recall)
max(DiskANN recall over accepted L_search/beam grid) >= min(max(IVF+RaBitQ recall), operator high-recall cap)
```

For the current ImageNet promotion, the operator high-recall cap is `0.995`; DiskANN does not need to chase IVF+RaBitQ's `0.999400` endpoint as long as the promoted single-index grid reaches at least `0.995`.

If the low end is too high, the runner should add lower `L_search` or beam values before building a different index. If the high end is too low, the runner may extend `L_search`/beam and then build a stronger index only if search parameters cannot recover the upper bound. FlatStor and Lance final rows must share the same DiskANN graph/index identity; only the payload backend timing changes.

Alternative considered: promote the best DiskANN rows across multiple indexes. That improves the curve but violates the user's requirement and makes build/index cost comparisons less interpretable.

## Risks / Trade-offs

- [Risk] ImageNet payload export can consume hundreds of GB if implemented by unpacking archives. -> Mitigation: stream tar contents directly into FlatStor/Lance and keep disk checks before each stage.
- [Risk] Lance export for large binary payloads may be slower or larger than FlatStor. -> Mitigation: export Lance after FlatStor, record output sizes, and keep failures isolated by backend.
- [Risk] Exact top-100 generation on ImageNet may be slow. -> Mitigation: use chunked matrix multiplication or FAISS exact search, write partial progress atomically, and validate shape before promotion.
- [Risk] DiskANN low-recall coverage may be hard if the graph is already too accurate at the lowest search setting. -> Mitigation: try lower `L_search`/beam first and record any unavoidable best-effort exception explicitly before promotion.
- [Risk] VoxCeleb2 speaker distribution may make query sampling biased. -> Mitigation: use the existing speaker-aware query selection manifest and persist the exact query ids.
- [Risk] Existing readiness checks assume Parquet backend presence. -> Mitigation: make required payload backends dataset-configurable and assert no Parquet rows appear in the new-dataset summaries.

## Migration Plan

1. Add registry entries and dataset defaults for `imagenet1k` and `voxceleb2_ecapa_150k`.
2. Implement formatted asset preparation and readiness checks for FlatStor/Lance-only datasets.
3. Generate exact ground truth and validate row counts, dimensions, normalization, and query split consistency.
4. Extend IVF and DiskANN runners to load the new datasets.
5. Run VoxCeleb2 first as the lower-risk end-to-end validation.
6. Run ImageNet after Vox passes readiness and smoke tests.
7. Keep DiskANN probe outputs separate until the one-index coverage gate passes.
8. Promote accepted rows into the ICDE baseline summaries, with backups before any replacement.

Rollback is file-level: remove only generated outputs from this change's new dataset directories or restore backed-up summary CSVs. Raw downloads and existing old-dataset results must remain untouched.

## Open Questions

- Should ImageNet's main payload include only JPEG bytes and metadata, or also derived image-size fields for payload bucket analysis in the same pass?
- Should VoxCeleb2 keep full audio bytes in Lance for the first baseline run, or use a metadata-plus-path payload for an additional appendix-only sensitivity check later?
