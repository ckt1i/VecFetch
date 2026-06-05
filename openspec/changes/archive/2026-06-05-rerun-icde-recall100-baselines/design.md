## Context

The paper experiment plan has moved the main top-k tiers from `Recall@10/50` to `Recall@10/100`. The local formal baseline assets are ready for five ICDE datasets (`coco_100k`, `msmarco_passage`, `amazon_esci`, `imagenet1k`, `voxceleb2_ecapa_150k`) and each has `gt_top100.npy`. The current ICDE suite, however, still hard-codes `TOPKS = [10, 50]`, filters aggregation to `{10,50}`, and does not emit explicit `recall@100` fields.

The existing DiskANN index directories were aggressively cleaned after the previous run. This change must not rebuild them. The refreshed result surface must use one existing DiskANN index per dataset and exclude any rows that reference other DiskANN graph identities.

## Goals / Non-Goals

**Goals:**

- Add a `Recall@100` rerun path for the five currently runnable ICDE formal baseline datasets.
- Keep the refreshed ICDE baseline surface to `topk=10` and `topk=100`.
- Rerun `topk=100` for IVF+PQ, IVF+RQ, and DiskANN with FlatStor and Lance.
- Reuse existing IVF/RaBitQ caches, payload backends, and DiskANN indexes.
- Enforce a no-build DiskANN policy and fail fast on missing or mismatched existing indexes.
- Produce auditable summary, selected, and report outputs that include `recall@100` and preserve old artifacts via backups.

**Non-Goals:**

- Do not guarantee or chase `Recall@100 >= 0.95`.
- Do not build new DiskANN graph indexes or resurrect previously deleted candidate indexes.
- Do not delete old `topk=50` result artifacts; just exclude them from the refreshed ICDE main summary.
- Do not add currently unready datasets such as `deep8m_synth`, `clotho`, or `msrvtt`.
- Do not run payload Parquet baselines.

## Decisions

### Decision 1: Make ICDE top-k tiers configurable, with refreshed default `{10,100}`

`run_icde_baseline_suite.py` should accept `--topks` so implementation can run only `100` for execution stages and `10,100` for aggregation. The refreshed default for this change should be `10,100`, while existing `topk=50` output files remain untouched on disk.

Alternative considered: replace every `50` constant with `100`. That is brittle because it prevents preserving and auditing historical `topk=50` outputs.

### Decision 2: Use `candidate_budget=topk*20` for the recall@100 rerun

The existing suite function already computes `candidate_budget(topk) = topk * 20`, and the updated tracker records that policy. Therefore `topk=100` uses `candidate_budget=2000` for IVF+PQ and IVF+RQ rerank runs. This keeps the `K=100` rerun consistent with the current execution code path and the updated experiment tracker.

Alternative considered: use the older formal-baseline spec's `topk=100 -> 500` budget. That would silently diverge from the current runner convention and from the new tracker.

### Decision 3: Add explicit `recall@100` fields while keeping `recall_at_topk` as the selection key

Shared metrics should emit `recall@100` for vector and E2E rows. DiskANN rows should also include `recall@100`. Aggregation should include the field in summary and selected outputs. `recall_at_topk` remains the canonical value for threshold matching because it already means "recall at the row's declared topk."

Alternative considered: infer `recall@100` from `recall_at_topk` only when `topk=100`. That works for selection but makes reports and downstream scripts harder to audit.

### Decision 4: DiskANN rerun uses one existing index per dataset and never builds

The DiskANN runner path must pass `--index-dir` and matching manifest parameters for these existing indexes:

| Dataset | Existing DiskANN index used for refreshed results |
| --- | --- |
| `coco_100k` | `/home/zcq/VDB/baselines/data/diskann_disk_coco_100k_R64_L100_PQ0` |
| `msmarco_passage` | `/home/zcq/VDB/baselines/data/diskann_disk_msmarco_passage_R32_L50_PQ0` |
| `amazon_esci` | `/home/zcq/VDB/baselines/data/diskann_disk_amazon_esci_R64_L100_PQ0` |
| `imagenet1k` | `/home/zcq/VDB/baselines/data/diskann_disk_imagenet1k_R13_L100_PQ0` |
| `voxceleb2_ecapa_150k` | `/home/zcq/VDB/baselines/data/diskann_disk_voxceleb2_ecapa_150k_R13_L100_PQ0` |

The implementation should add a suite-level `--no-build-diskann` or equivalent hard guard. If an expected index is missing, if manifest identity mismatches `dataset/R/L/pq_disk_bytes`, or if the runner would create a new default index directory, the run must stop before scheduling DiskANN.

Alternative considered: use `diskann_defaults()` and let the runner build if the default index is missing. This violates the user's storage constraint and would recreate deleted candidate indexes.

### Decision 5: Aggregate best-effort recall@100 results without treating 0.95 as required

The selected table should still attempt thresholds `0.80/0.90/0.95` for `topk=100`, but if a group does not reach a threshold, selection should mark `best-effort` and choose the highest achieved recall row for that group. The report should summarize missing thresholds rather than blocking the rerun.

Alternative considered: require every dataset/system/backend to reach `0.95`. The user explicitly removed that guarantee for this change.

## Risks / Trade-offs

- [Risk] Existing DiskANN indexes may not span enough `Recall@100` range. -> Mitigation: record best-effort rows and do not rebuild; the result is still useful for comparison under the no-build constraint.
- [Risk] `topk=100` with `candidate_budget=2000` may be slow on MS MARCO. -> Mitigation: run smaller datasets first and preserve partial outputs before scheduling MS MARCO.
- [Risk] Aggregation could accidentally merge old `topk=50` rows. -> Mitigation: add validation that refreshed summary and selected outputs contain no `topk=20` or `topk=50`.
- [Risk] Amazon ESCI has two existing DiskANN indexes on disk. -> Mitigation: the refreshed result surface uses only `R64_L100_PQ0`; old `R32_L50_PQ0` may remain on disk but is excluded from refreshed summary rows.
- [Risk] Adding fields can break CSV parsing. -> Mitigation: keep existing fields, append `recall@100`, and back up prior summary/selected/report files before replacement.

## Migration Plan

1. Patch metric schemas and recall evaluation to emit `recall@100`.
2. Patch the ICDE suite to accept `--topks`, schedule `topk=100`, aggregate `topk=10/100`, and validate that refreshed outputs exclude `20/50`.
3. Patch DiskANN suite execution to use the fixed existing-index map and no-build guard.
4. Dry-run the command matrix for all five datasets and verify no DiskANN build commands are scheduled.
5. Run `topk=100` first on `coco_100k` and `voxceleb2_ecapa_150k`, aggregate, and validate output schema.
6. Run `amazon_esci` and `imagenet1k`, aggregate, and validate the single-index rule.
7. Run `msmarco_passage` last, aggregate with all five datasets, and write the final report.
8. Leave old `topk=50` artifacts in place; rollback is restoring backed-up summary/selected/report files and removing only newly generated `topk=100` outputs if needed.
