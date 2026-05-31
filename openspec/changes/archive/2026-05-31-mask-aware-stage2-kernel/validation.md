## Baseline Before Masked Kernel

Command:

```bash
./build/benchmarks/bench_e2e \
  --dataset /home/zcq/VDB/test/msmarco_fht_kac_adapter \
  --gt-file /home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/gt/gt_top10.npy \
  --index-dir /home/zcq/VDB/test/data/MSMARCO/fht_kac_rotator \
  --output /home/zcq/VDB/test/mask_aware_stage2_baseline/before \
  --query-only 0 --skip-gt 0 --queries 200 --topk 10 --nprobe 256 \
  --clu-read-mode full_preload --use-resident-clusters 1 \
  --io-queue-depth 64 --cluster-submit-reserve 8 \
  --prefetch-depth 16 --initial-prefetch 16 \
  --refill-threshold 4 --refill-count 8 --submit-batch 32 \
  --early-stop 0 --crc 1 \
  --fine-grained-timing 0 --hotpath-detailed-timing 0 \
  --fixed-vec-buffer-count 512 \
  --two-level-coarse-routing 1 \
  --two-level-coarse-super-factor 2 \
  --two-level-coarse-budget-factor 12
```

Output:

`/home/zcq/VDB/test/mask_aware_stage2_baseline/before/msmarco_fht_kac_adapter_20260529T202046`

Summary:

- `recall@1=0.9600`
- `recall@5=0.9490`
- `recall@10=0.9435`
- `avg_query=4.930 ms`
- `probe=3.834 ms`
- `probe_stage1=1.596 ms`
- `probe_stage2=1.000 ms`
- `probe_submit=0.880 ms`
- `io_wait=0.001 ms`

## After Masked Kernel

Command:

```bash
./build/benchmarks/bench_e2e \
  --dataset /home/zcq/VDB/test/msmarco_fht_kac_adapter \
  --gt-file /home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/gt/gt_top10.npy \
  --index-dir /home/zcq/VDB/test/data/MSMARCO/fht_kac_rotator \
  --output /home/zcq/VDB/test/mask_aware_stage2_baseline/after \
  --query-only 0 --skip-gt 0 --queries 200 --topk 10 --nprobe 256 \
  --clu-read-mode full_preload --use-resident-clusters 1 \
  --io-queue-depth 64 --cluster-submit-reserve 8 \
  --prefetch-depth 16 --initial-prefetch 16 \
  --refill-threshold 4 --refill-count 8 --submit-batch 32 \
  --early-stop 0 --crc 1 \
  --fine-grained-timing 0 --hotpath-detailed-timing 0 \
  --fixed-vec-buffer-count 512 \
  --two-level-coarse-routing 1 \
  --two-level-coarse-super-factor 2 \
  --two-level-coarse-budget-factor 12
```

Output:

`/home/zcq/VDB/test/mask_aware_stage2_baseline/after/msmarco_fht_kac_adapter_20260529T202755`

Summary:

- `recall@1=0.9600`
- `recall@5=0.9490`
- `recall@10=0.9435`
- `avg_query=4.810 ms`
- `probe=3.734 ms`
- `probe_stage1=1.550 ms`
- `probe_stage2=0.935 ms`
- `probe_submit=0.888 ms`
- `io_wait=0.003 ms`
- `avg_stage2_masked_kernel_calls=1314.465`
- `avg_stage2_lanes_requested=2759.135`
- `avg_stage2_lanes_skipped=7726.870`
- `avg_stage2_lanes_total_valid=10486.005`
- `avg_stage2_lane_density=0.2631`

Comparison:

- recall unchanged: `recall@10 0.9435 -> 0.9435`
- avg query improved by `0.120 ms/query`: `4.930 -> 4.810`
- probe improved by `0.100 ms/query`: `3.834 -> 3.734`
- low-overhead stage2 split improved by `0.065 ms/query`: `1.000 -> 0.935`
- masked kernel skipped about `73.7%` of valid Stage2 lanes in this run

## Perf After

Command:

```bash
perf record -F 999 -g \
  -o /home/zcq/VDB/test/mask_aware_stage2_baseline/perf_after/perf.data \
  -- ./build/benchmarks/bench_e2e ...
```

Output:

`/home/zcq/VDB/test/mask_aware_stage2_baseline/perf_after/perf.data`

Notes:

- This perf run records the full benchmark including preload and CRC calibration, so its `avg_query` is perturbed and not used as the speed conclusion.
- Query recall still matched: `recall@10=0.9435`.
- `perf report` shows `IPExRaBitQBatchPackedSignParallelCompactMasked` under `OverlapScheduler::ProbeCluster`.
- No significant samples remain in the old `IPExRaBitQBatchPackedSignParallelCompact` symbol in the filtered report.
- Full-run no-children sample share:
  - `IPExRaBitQBatchPackedSignParallelCompactMasked`: about `0.78%`
  - `RaBitQEstimator::EstimateDistanceFastScan`: about `0.73%`
  - `OverlapScheduler::EmitPendingDataRequests`: about `0.01%`
