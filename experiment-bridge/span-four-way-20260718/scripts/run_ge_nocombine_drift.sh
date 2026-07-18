#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/zcq/VDB}
BINARY=${BINARY:-/tmp/vdb-span-four-way-build/benchmarks/bench_e2e}
INPUT=${INPUT:-$ROOT/test/recordgate_safein_confidence_bextra_20260712/inputs}
EPS=${EPS:-$ROOT/test/recordgate_hybrid_split_static_safein_20260712/runtime_epsilon_cache}
COMPACT=${COMPACT:-$ROOT/test/recordgate_vec_span_stage1_20260715/stores}
NOCOMBINE=${NOCOMBINE:-$ROOT/test/recordgate_nocombine_safein_current_span_20260717/stores}
OUT_ROOT=${OUT_ROOT:-$ROOT/test/recordgate_span_ge_nocombine_drift_20260718}
DATASETS=${DATASETS:-"coco_100k msmarco_passage amazon_esci voxceleb2_ecapa_150k"}
REPS=${REPS:-3}
QUERIES=${QUERIES:-500}
WARMUP_QUERIES=${WARMUP_QUERIES:-500}
CPU=${CPU:-8}

combined_store() {
  case "$1" in
    coco_100k|amazon_esci)
      echo "$COMPACT/$1/compact_g4_prefix0_align8" ;;
    voxceleb2_ecapa_150k)
      echo "$ROOT/test/recordgate_p0_p1_auto_review_20260717/stores/$1/prefix32k" ;;
    msmarco_passage)
      echo "$COMPACT/$1/compact_g4_prefix0_align16" ;;
  esac
}
dataset_dir() {
  [[ "$1" == coco_100k ]] && echo "$ROOT/data/coco_100k" || \
    echo "$ROOT/data/bench_e2e/$1"
}
query_file() {
  [[ "$1" == coco_100k ]] && \
    echo "$ROOT/test/data/coco_100k/query_split_v1/query_embeddings.npy" || \
    echo "$INPUT/$1/holdout/query_embeddings.npy"
}
query_ids() {
  [[ "$1" == coco_100k ]] && \
    echo "$ROOT/test/data/coco_100k/query_split_v1/query_ids.npy" || \
    echo "$INPUT/$1/holdout/query_ids.npy"
}
ground_truth() {
  [[ "$1" == coco_100k ]] && \
    echo "$ROOT/test/data/coco_100k/query_split_v1/gt_top100_image_id.npy" || \
    echo "$INPUT/$1/holdout/gt_top100.npy"
}
query_offset() { [[ "$1" == coco_100k ]] && echo 500 || echo 0; }
nprobe_for() {
  case "$1" in
    msmarco_passage|voxceleb2_ecapa_150k) echo 96 ;;
    amazon_esci) echo 192 ;;
    coco_100k) echo 128 ;;
  esac
}
coarse_cap() {
  case "$1" in
    coco_100k) echo 3072 ;;
    amazon_esci) echo 6144 ;;
    msmarco_passage) echo 12288 ;;
    voxceleb2_ecapa_150k) echo 1536 ;;
  esac
}
prefetch_count() { [[ "$1" == voxceleb2_ecapa_150k ]] && echo 4 || echo 8; }
prefetch_bytes() { [[ "$1" == voxceleb2_ecapa_150k ]] && echo 32768 || echo 4096; }

validate_result() {
  jq -e '
    .metrics as $c | .pipeline_stats as $p |
    $c.vec_span_planner_mode == "GE" and
    $c.vec_span_alpha_num == 3 and $c.vec_span_alpha_den == 2 and
    $c.vec_span_safein_rho_num == 0 and
    $c.vec_span_safein_tail_count == 0 and
    $p.avg_vec_span_planner_fallbacks == 0 and
    $p.avg_vec_span_planned_physical_bytes == $p.avg_vec_only_read_bytes and
    $p.avg_vec_span_planner_groups == $p.avg_vec_only_read_requests
  ' "$1" >/dev/null
}

run_one() {
  local ds=$1 variant=$2 output=$3 queries=$4 cache_mode=$5
  local store count bytes extra materialization reuse cold
  local -a layout
  store=$(combined_store "$ds")
  count=$(prefetch_count "$ds")
  bytes=$(prefetch_bytes "$ds")
  extra=$((count * bytes))
  if [[ "$variant" == Combined ]]; then
    layout=(--inline-hot-record-store-dir "$store")
    materialization=late
    reuse=1
    cold=0
    count=0
    bytes=0
    extra=0
  else
    layout=(--separate-store-dir "$NOCOMBINE/$ds")
    materialization=eager
    reuse=0
    cold=1
  fi
  mkdir -p "$output"
  VDB_RESIDENT_HUGEPAGE=0 taskset -c "$CPU" "$BINARY" \
    --dataset "$(dataset_dir "$ds")" --query-file "$(query_file "$ds")" \
    --query-ids "$(query_ids "$ds")" --gt-file "$(ground_truth "$ds")" \
    --query-offset "$(query_offset "$ds")" --gt-offset "$(query_offset "$ds")" \
    --queries "$queries" --topk 100 --nprobe "$(nprobe_for "$ds")" \
    --index-dir "$store" "${layout[@]}" --output "$output" \
    --rabitq-validation-mode official_1_plus_n \
    --rabitq-active-bits 4 --rabitq-resident-bits 4 \
    --dynamic-safeout 1 --dynamic-safein frontier \
    --materialization-mode "$materialization" --safein-threshold-bytes 0 \
    --safein-prefetch-order confidence --safein-prefetch-rank-batch-size 32 \
    --safein-prefetch-global-window 0 --safein-prefetch-max-count "$count" \
    --safein-prefetch-max-bytes 0 --safein-query-extra-bytes "$extra" \
    --safein-max-full-payload-bytes "$bytes" \
    --safein-cold-payload-prefetch "$cold" \
    --safein-cold-payload-prefix-bytes "$bytes" \
    --payload-cache-mode "$cache_mode" --skip-false-stats 1 \
    --safeout-epsilon-percentile 0.99 --epsilon-samples 100 \
    --epsilon-sampling-mode legacy_per_cluster \
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
  [[ -s "$(combined_store "$ds")/manifest.json" ]] || exit 2
  [[ -s "$NOCOMBINE/$ds/manifest.json" ]] || exit 2
  for rep in $(seq 1 "$REPS"); do
    if ((rep % 2)); then variants="Combined NoCombine"; else variants="NoCombine Combined"; fi
    for variant in $variants; do
      output="$OUT_ROOT/runs/$ds/k100/np$(nprobe_for "$ds")/$variant/rep$rep"
      [[ -s "$output/results.json" ]] && { validate_result "$output/results.json"; continue; }
      warm="$OUT_ROOT/warmup/$ds/$variant/rep$rep"
      run_one "$ds" "$variant" "$warm" "$WARMUP_QUERIES" default \
        >"$OUT_ROOT/${ds}_${variant}_warm_r${rep}.log" 2>&1
      run_one "$ds" "$variant" "$output" "$QUERIES" drop-before-queries \
        >"$OUT_ROOT/${ds}_${variant}_r${rep}.log" 2>&1
    done
  done
done
