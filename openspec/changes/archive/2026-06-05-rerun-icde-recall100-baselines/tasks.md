## 1. Metric and Suite Configuration

- [x] 1.1 Add `recall@100` to shared vector-search and coupled E2E metric field lists.
- [x] 1.2 Update recall metric computation so IVF and E2E rows emit `recall@100` alongside `recall_at_topk` and `recall@10`.
- [x] 1.3 Update DiskANN CSV header and row writing to emit `recall@100`.
- [x] 1.4 Add a `--topks` option to `run_icde_baseline_suite.py`.
- [x] 1.5 Change refreshed ICDE defaults and aggregation filters from `{10,50}` to `{10,100}`.
- [x] 1.6 Keep old `topk=50` files on disk but prevent them from entering refreshed summary, selected, and report outputs.

## 2. Existing-Index DiskANN Execution

- [x] 2.1 Add an accepted DiskANN index map for `coco_100k`, `msmarco_passage`, `amazon_esci`, `imagenet1k`, and `voxceleb2_ecapa_150k`.
- [x] 2.2 Configure `amazon_esci` to use only `diskann_disk_amazon_esci_R64_L100_PQ0` in refreshed results.
- [x] 2.3 Ensure DiskANN suite commands pass explicit `--index-dir`, matching `--R`, matching `--L`, and `--pq-disk-bytes 0`.
- [x] 2.4 Add a no-build guard so missing or mismatched DiskANN indexes fail before invoking `build_disk_index`.
- [x] 2.5 Dry-run the DiskANN command matrix and verify no command would create or force-build a new index.

## 3. Recall@100 Baseline Rerun

- [x] 3.1 Verify all five target datasets are phase0-ready and have `gt_top100.npy`.
- [x] 3.2 Run `topk=100` IVF+PQ FlatStor/Lance for all five target datasets.
- [x] 3.3 Run `topk=100` IVF+RQ FlatStor/Lance for all five target datasets, reusing vector-search output across backends.
- [x] 3.4 Run `topk=100` DiskANN FlatStor/Lance for all five target datasets using only accepted existing indexes.
- [x] 3.5 Run smaller datasets first (`coco_100k`, `voxceleb2_ecapa_150k`), then `amazon_esci` and `imagenet1k`, then `msmarco_passage`.
- [x] 3.6 Stop and report before continuing if disk space becomes unsafe or a required existing index is missing.

## 4. Aggregation and Validation

- [x] 4.1 Aggregate refreshed ICDE outputs with `topk=10,100`.
- [x] 4.2 Back up existing `icde_baseline_summary.csv`, `icde_baseline_selected.csv`, and `icde_baseline_report.md` before replacement.
- [x] 4.3 Verify refreshed summary and selected outputs contain no `topk=20` or `topk=50` rows.
- [x] 4.4 Verify every `dataset × system × backend × topk=100` group has at least one valid row or an explicit failure record.
- [x] 4.5 Verify DiskANN refreshed rows have exactly one non-empty `index_id` per dataset.
- [x] 4.6 Verify all DiskANN refreshed rows have `pq_disk_bytes=0` and reference only accepted existing index directories.
- [x] 4.7 Verify selected rows mark unreached `Recall@100` thresholds as `best-effort` instead of failing the run.
- [x] 4.8 Update the Markdown report to state that `0.995` is the highest `Recall@100` selection target and not a hard pass requirement.
- [x] 4.9 Record final disk space, remaining running processes, output paths, and any best-effort threshold gaps.
