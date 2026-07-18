#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/zcq/VDB}
BINARY=${BINARY:-/tmp/vdb-span-four-way-build/benchmarks/bench_e2e}
INPUT_ROOT=${INPUT_ROOT:-$ROOT/test/recordgate_safein_confidence_bextra_20260712/inputs}
EPS_ROOT=${EPS_ROOT:-$ROOT/test/recordgate_hybrid_split_static_safein_20260712/runtime_epsilon_cache}
STORE_ROOT=${STORE_ROOT:-$ROOT/test/recordgate_vec_span_stage1_20260715/stores}
VOX_STORE=${VOX_STORE:-$ROOT/test/recordgate_p0_p1_auto_review_20260717/stores/voxceleb2_ecapa_150k/prefix32k}
OUT_ROOT=${OUT_ROOT:-$ROOT/test/recordgate_span_safein_prefetch_noreuse_ablation_20260718}
DATASETS=${DATASETS:-"amazon_esci coco_100k msmarco_passage voxceleb2_ecapa_150k"}
REPS=${REPS:-5}
QUERIES=${QUERIES:-500}
WARMUP_QUERIES=${WARMUP_QUERIES:-500}
NPROBE=${NPROBE:-96}
CPU=${CPU:-8}

store_for() {
  case "$1" in
    amazon_esci|coco_100k) echo "$STORE_ROOT/$1/compact_g4_prefix0_align8" ;;
    msmarco_passage) echo "$STORE_ROOT/$1/compact_g4_prefix0_align16" ;;
    voxceleb2_ecapa_150k) echo "$VOX_STORE" ;;
    *) echo "unknown dataset: $1" >&2; return 2 ;;
  esac
}

dataset_dir() {
  [[ "$1" == coco_100k ]] && echo "$ROOT/data/coco_100k" || \
    echo "$ROOT/data/bench_e2e/$1"
}

query_file() {
  [[ "$1" == coco_100k ]] && \
    echo "$ROOT/test/data/coco_100k/query_split_v1/query_embeddings.npy" || \
    echo "$INPUT_ROOT/$1/holdout/query_embeddings.npy"
}

query_ids() {
  [[ "$1" == coco_100k ]] && \
    echo "$ROOT/test/data/coco_100k/query_split_v1/query_ids.npy" || \
    echo "$INPUT_ROOT/$1/holdout/query_ids.npy"
}

ground_truth() {
  [[ "$1" == coco_100k ]] && \
    echo "$ROOT/test/data/coco_100k/query_split_v1/gt_top100_image_id.npy" || \
    echo "$INPUT_ROOT/$1/holdout/gt_top100.npy"
}

query_offset() { [[ "$1" == coco_100k ]] && echo 500 || echo 0; }

coarse_cap() {
  case "$1" in
    amazon_esci) echo 6144 ;;
    coco_100k) echo 3072 ;;
    msmarco_passage) echo 12288 ;;
    voxceleb2_ecapa_150k) echo 1536 ;;
    *) return 2 ;;
  esac
}

validate_result() {
  local result_file=$1 variant=$2
  local expected_vec_only=false expected_materialization=eager
  if [[ "$variant" == NoSafeInPrefetch ]]; then
    expected_vec_only=true
    expected_materialization=late
  fi
  jq -e --argjson expected_vec_only "$expected_vec_only" \
      --arg expected_materialization "$expected_materialization" '
    .metrics as $c | .pipeline_stats as $p |
    $c.record_layout == "inline_hot_record_store" and
    $c.vec_span_planner_mode == "GE" and
    $c.vec_span_coalescing_enabled == true and
    $c.vec_span_alpha_num == 3 and $c.vec_span_alpha_den == 2 and
    $c.vec_span_safein_rho_num == 0 and $c.vec_span_safein_rho_den == 1 and
    $c.vec_span_payload_reuse_enabled == false and
    $c.vec_span_safein_tail_count == 0 and
    $c.materialization_mode == $expected_materialization and
    $c.safein_as_vec_only == $expected_vec_only and
    $p.avg_vec_span_planner_fallbacks == 0 and
    $p.avg_vec_span_planner_credit_bytes == 0 and
    $p.avg_vec_span_payload_views == 0 and
    $p.avg_vec_span_payload_view_bytes == 0 and
    $p.avg_vec_span_payload_retained_bytes == 0 and
    $p.avg_vec_span_payload_reuse_hits == 0 and
    $p.avg_vec_span_payload_reuse_bytes == 0 and
    $p.avg_vec_span_payload_requests_avoided == 0 and
    $p.avg_vec_span_planned_physical_bytes == $p.avg_vec_only_read_bytes and
    $p.avg_vec_span_planner_groups == $p.avg_vec_only_read_requests and
    (if $expected_vec_only then $p.avg_all_read_requests == 0
     else $p.avg_all_read_requests > 0 end)
  ' "$result_file" >/dev/null || {
    echo "invalid SafeIn-prefetch no-reuse contract: $result_file" >&2
    return 3
  }
}

run_one() {
  local dataset=$1 variant=$2 output=$3 queries=$4 cache_mode=$5
  local store safein_vec_only=0 materialization=eager log
  store=$(store_for "$dataset")
  if [[ "$variant" == NoSafeInPrefetch ]]; then
    safein_vec_only=1
    materialization=late
  fi
  log=${output%/}/run.log
  mkdir -p "$output"
  VDB_RESIDENT_HUGEPAGE=0 taskset -c "$CPU" "$BINARY" \
    --dataset "$(dataset_dir "$dataset")" \
    --query-file "$(query_file "$dataset")" \
    --query-ids "$(query_ids "$dataset")" \
    --gt-file "$(ground_truth "$dataset")" \
    --query-offset "$(query_offset "$dataset")" \
    --gt-offset "$(query_offset "$dataset")" \
    --queries "$queries" --topk 100 --nprobe "$NPROBE" \
    --index-dir "$store" --inline-hot-record-store-dir "$store" \
    --output "$output" --rabitq-validation-mode official_1_plus_n \
    --rabitq-active-bits 4 --rabitq-resident-bits 4 \
    --dynamic-safeout 1 --dynamic-safein frontier \
    --materialization-mode "$materialization" \
    --safein-as-vec-only "$safein_vec_only" \
    --safein-threshold-bytes 0 --safein-prefetch-order confidence \
    --safein-prefetch-rank-batch-size 32 --safein-prefetch-global-window 0 \
    --safein-prefetch-max-count 0 --safein-prefetch-max-bytes 0 \
    --safein-query-extra-bytes 0 --safein-max-full-payload-bytes 0 \
    --safein-cold-payload-prefetch 0 --payload-cache-mode "$cache_mode" \
    --skip-false-stats 1 --safeout-epsilon-percentile 0.99 \
    --epsilon-samples 100 --epsilon-sampling-mode legacy_per_cluster \
    --safeout-epsilon-cache "$EPS_ROOT/${dataset}_safeout_p0p99_s100_legacy_per_cluster.txt" \
    --io-queue-depth 64 --fixed-vec-buffer-count 1024 \
    --cluster-submit-reserve 8 --submit-batch 32 \
    --submission-mode shared --sqpoll 0 --iopoll 0 \
    --vec-span-coalescing 1 --vec-span-tile-bytes 65536 \
    --vec-span-planner-mode GE --vec-span-alpha=3/2 \
    --vec-span-safein-rho=0 --vec-span-payload-reuse 0 \
    --vec-span-payload-compact 0 --vec-span-safein-tail-count 0 \
    --two-level-coarse-routing 1 --two-level-coarse-threshold 4096 \
    --two-level-coarse-budget-factor 16 \
    --two-level-coarse-budget-cap "$(coarse_cap "$dataset")" \
    >"$log" 2>&1
  validate_result "$output/results.json" "$variant"
}

[[ -x "$BINARY" ]] || { echo "missing binary: $BINARY" >&2; exit 1; }
mkdir -p "$OUT_ROOT"
for rep in $(seq 1 "$REPS"); do
  for dataset in $DATASETS; do
    [[ -s "$(store_for "$dataset")/manifest.json" ]] || {
      echo "missing inline store for $dataset" >&2
      exit 2
    }
    warm="$OUT_ROOT/rep$rep/warmup/q$WARMUP_QUERIES/$dataset/GE"
    [[ -s "$warm/results.json" ]] || \
      run_one "$dataset" NoSafeInPrefetch "$warm" "$WARMUP_QUERIES" default
    if ((rep % 2)); then
      variants="NoSafeInPrefetch SafeInPrefetch"
    else
      variants="SafeInPrefetch NoSafeInPrefetch"
    fi
    for variant in $variants; do
      measured="$OUT_ROOT/rep$rep/runs/q$QUERIES/$dataset/$variant"
      if [[ ! -s "$measured/results.json" ]]; then
        run_one "$dataset" "$variant" "$measured" "$QUERIES" drop-before-queries
      else
        validate_result "$measured/results.json" "$variant"
      fi
      echo "[done] rep=$rep dataset=$dataset variant=$variant"
    done
  done
done

echo "SafeIn-prefetch no-reuse results: $OUT_ROOT"
