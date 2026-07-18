#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/zcq/VDB}
BINARY=${BINARY:-/tmp/vdb-span-four-way-build/benchmarks/bench_e2e}
INPUT=${INPUT:-$ROOT/test/recordgate_safein_confidence_bextra_20260712/inputs}
EPS=${EPS:-$ROOT/test/recordgate_hybrid_split_static_safein_20260712/runtime_epsilon_cache}
COMPACT=${COMPACT:-$ROOT/test/recordgate_vec_span_stage1_20260715/stores}
OUT_ROOT=${OUT_ROOT:-$ROOT/test/recordgate_span_ge_pipeline_drift_20260718}
DATASETS=${DATASETS:-"coco_100k msmarco_passage voxceleb2_ecapa_150k"}
REPS=${REPS:-3}
QUERIES=${QUERIES:-500}
WARMUP_QUERIES=${WARMUP_QUERIES:-500}
CPU=${CPU:-8}

store_for() {
  case "$1" in
    coco_100k|voxceleb2_ecapa_150k) echo "$COMPACT/$1/compact_g4_prefix0_align8" ;;
    msmarco_passage) echo "$COMPACT/$1/compact_g4_prefix0_align16" ;;
  esac
}
dataset_dir() { [[ "$1" == coco_100k ]] && echo "$ROOT/data/coco_100k" || echo "$ROOT/data/bench_e2e/$1"; }
query_file() { [[ "$1" == coco_100k ]] && echo "$ROOT/test/data/coco_100k/query_split_v1/query_embeddings.npy" || echo "$INPUT/$1/holdout/query_embeddings.npy"; }
query_ids() { [[ "$1" == coco_100k ]] && echo "$ROOT/test/data/coco_100k/query_split_v1/query_ids.npy" || echo "$INPUT/$1/holdout/query_ids.npy"; }
ground_truth() { [[ "$1" == coco_100k ]] && echo "$ROOT/test/data/coco_100k/query_split_v1/gt_top100_image_id.npy" || echo "$INPUT/$1/holdout/gt_top100.npy"; }
query_offset() { [[ "$1" == coco_100k ]] && echo 500 || echo 0; }
coarse_cap() { case "$1" in coco_100k) echo 3072;; msmarco_passage) echo 12288;; voxceleb2_ecapa_150k) echo 1536;; esac; }

validate_result() {
  jq -e '
    .metrics as $c | .pipeline_stats as $p |
    $c.vec_span_planner_mode == "GE" and
    $c.vec_span_alpha_num == 3 and $c.vec_span_alpha_den == 2 and
    $c.vec_span_safein_tail_count == 0 and
    $p.avg_vec_span_planner_fallbacks == 0 and
    $p.avg_vec_span_planned_physical_bytes == $p.avg_vec_only_read_bytes and
    $p.avg_vec_span_planner_groups == $p.avg_vec_only_read_requests
  ' "$1" >/dev/null
}

run_one() {
  local ds=$1 variant=$2 output=$3 queries=$4 cache_mode=$5
  local execution=overlap reuse=1 safein_vec_only=0 store
  store=$(store_for "$ds")
  case "$variant" in
    FullCurrent) ;;
    OldNoOverlap) execution=serial-no-overlap ;;
    NewNoPipeline) execution=serial-no-overlap; reuse=0; safein_vec_only=1 ;;
  esac
  mkdir -p "$output"
  VDB_RESIDENT_HUGEPAGE=0 taskset -c "$CPU" "$BINARY" \
    --dataset "$(dataset_dir "$ds")" --query-file "$(query_file "$ds")" \
    --query-ids "$(query_ids "$ds")" --gt-file "$(ground_truth "$ds")" \
    --query-offset "$(query_offset "$ds")" --gt-offset "$(query_offset "$ds")" \
    --queries "$queries" --topk 100 --nprobe 96 --index-dir "$store" \
    --inline-hot-record-store-dir "$store" --output "$output" \
    --execution-mode "$execution" --safein-as-vec-only "$safein_vec_only" \
    --rabitq-validation-mode official_1_plus_n \
    --rabitq-active-bits 4 --rabitq-resident-bits 4 \
    --dynamic-safeout 1 --dynamic-safein frontier --materialization-mode late \
    --safein-threshold-bytes 0 --safein-prefetch-order confidence \
    --safein-prefetch-rank-batch-size 32 --safein-prefetch-global-window 0 \
    --safein-prefetch-max-count 0 --safein-prefetch-max-bytes 0 \
    --safein-query-extra-bytes 0 --safein-max-full-payload-bytes 0 \
    --safein-cold-payload-prefetch 0 --payload-cache-mode "$cache_mode" \
    --skip-false-stats 1 --safeout-epsilon-percentile 0.99 \
    --epsilon-samples 100 --epsilon-sampling-mode legacy_per_cluster \
    --safeout-epsilon-cache "$EPS/${ds}_safeout_p0p99_s100_legacy_per_cluster.txt" \
    --io-queue-depth 64 --fixed-vec-buffer-count 1024 \
    --cluster-submit-reserve 8 --submit-batch 32 --submission-mode shared \
    --sqpoll 0 --iopoll 0 --vec-span-coalescing 1 \
    --vec-span-tile-bytes 65536 --vec-span-planner-mode GE \
    --vec-span-alpha=3/2 --vec-span-safein-rho=0 \
    --vec-span-payload-reuse "$reuse" --vec-span-payload-compact 0 \
    --vec-span-safein-tail-count 0 --two-level-coarse-routing 1 \
    --two-level-coarse-threshold 4096 --two-level-coarse-budget-factor 16 \
    --two-level-coarse-budget-cap "$(coarse_cap "$ds")"
  validate_result "$output/results.json"
}

[[ -x "$BINARY" ]] || { echo "missing binary: $BINARY" >&2; exit 1; }
mkdir -p "$OUT_ROOT"
for ds in $DATASETS; do
  [[ -s "$(store_for "$ds")/manifest.json" ]] || exit 2
  for rep in $(seq 1 "$REPS"); do
    if ((rep % 2)); then variants="FullCurrent OldNoOverlap NewNoPipeline"; else variants="NewNoPipeline OldNoOverlap FullCurrent"; fi
    for variant in $variants; do
      output="$OUT_ROOT/runs/$ds/k100/np96/$variant/rep$rep"
      [[ -s "$output/results.json" ]] && { validate_result "$output/results.json"; continue; }
      warm="$OUT_ROOT/warmup/$ds/$variant/rep$rep"
      run_one "$ds" "$variant" "$warm" "$WARMUP_QUERIES" default \
        >"$OUT_ROOT/${ds}_${variant}_warm_r${rep}.log" 2>&1
      run_one "$ds" "$variant" "$output" "$QUERIES" drop-before-queries \
        >"$OUT_ROOT/${ds}_${variant}_r${rep}.log" 2>&1
    done
  done
done
