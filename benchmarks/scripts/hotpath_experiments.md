# Hot-Path Experiments

This directory now contains a thin experiment harness for `bench_e2e`.

## What It Runs

- The default matrix is the hot-path experiment plan v1:
  - `fixed-probe-baseline`: `nprobe=64`, `--crc 1`, `--early-stop 0`
  - `crc-early-stop-baseline`: `nprobe=256`, `--crc 1`, `--early-stop 1`, `--crc-alpha 0.02`
- The current plan-v1 control variants also include:
  - `fixed-probe-submit-batch-0`
  - `fixed-probe-submit-batch-32`
- The older exploratory matrix is still available via `--mode legacy-baseline`.
- Heavier `Stage2 compute-ready` variants are intentionally left out because
  they require future code support and are not the current priority.

## Usage

```bash
python3 benchmarks/scripts/run_hotpath_experiments.py \
  --bench-bin ./build/benchmarks/bench_e2e \
  --dataset /home/zcq/VDB/data/coco_100k \
  --index-dir /home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90 \
  --output-root /home/zcq/VDB/test/hotpath_experiments \
  --queries 1000 \
  --topk 10 \
  --bits 4 \
  --repeats 3
```

Add `--run-perf` to collect `perf stat` for each run when `perf` is available.

To run only the two official baselines:

```bash
python3 benchmarks/scripts/run_hotpath_experiments.py \
  --bench-bin ./build/benchmarks/bench_e2e \
  --dataset /home/zcq/VDB/data/coco_100k \
  --index-dir /home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90 \
  --output-root /home/zcq/VDB/test/hotpath_experiments_plan_v1 \
  --queries 1000 \
  --topk 10 \
  --bits 4 \
  --repeats 3 \
  --mode plan-v1-baselines
```

## Outputs

- `hotpath_experiments.csv`: flat per-run summary
- `summary.json` under each experiment directory: mean/std for `avg_query_ms`
- `bench_output/results.json`: the raw benchmark output for each repeat

The CSV now also records:
- experiment profile and Stage2 breakdown mode
- `avg_probe_submit_ms` plus submit-window fields
- `avg_probe_stage2_ms` plus Stage2 collect/kernel/scatter fields
- CRC-path observability such as `avg_probed_clusters`, `early_stopped_pct`, and `avg_crc_would_stop`

## Interpretation

Treat a change as promising only if:

- `recall@10` stays stable, and
- `avg_query_ms` improvement is larger than run-to-run noise across repeats.

When Stage2 sub-fields are all zero while `avg_probe_stage2_ms` is non-zero, treat
that result as `low_overhead_or_unmeasured`: the benchmark is reporting total
Stage2 time, but not fine-grained collect/kernel/scatter attribution for that run.
