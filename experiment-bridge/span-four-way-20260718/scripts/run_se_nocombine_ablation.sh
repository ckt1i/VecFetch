#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/zcq/VDB}
BINARY=${BINARY:-/tmp/vdb-span-four-way-build/benchmarks/bench_e2e}
INPUT_ROOT=${INPUT_ROOT:-$ROOT/test/recordgate_safein_confidence_bextra_20260712/inputs}
EPS_ROOT=${EPS_ROOT:-$ROOT/test/recordgate_hybrid_split_static_safein_20260712/runtime_epsilon_cache}
COMPACT_ROOT=${COMPACT_ROOT:-$ROOT/test/recordgate_vec_span_stage1_20260715/stores}
VOX_STORE=${VOX_STORE:-$ROOT/test/recordgate_p0_p1_auto_review_20260717/stores/voxceleb2_ecapa_150k/prefix32k}
NOCOMBINE_ROOT=${NOCOMBINE_ROOT:-$ROOT/test/recordgate_nocombine_safein_current_span_20260717/stores}
OUT_ROOT=${OUT_ROOT:-$ROOT/test/recordgate_span_se_nocombine_ablation_20260718}
DATASETS=${DATASETS:-"amazon_esci coco_100k msmarco_passage voxceleb2_ecapa_150k"}
RHO=${RHO:-1/10}
REPS=${REPS:-5}
QUERIES=${QUERIES:-500}
WARMUP_QUERIES=${WARMUP_QUERIES:-500}
CPU=${CPU:-8}

combined_store() {
  case "$1" in
    amazon_esci|coco_100k) echo "$COMPACT_ROOT/$1/compact_g4_prefix0_align8" ;;
    msmarco_passage) echo "$COMPACT_ROOT/$1/compact_g4_prefix0_align16" ;;
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

nprobe_for() {
  case "$1" in
    amazon_esci) echo 192 ;;
    coco_100k) echo 128 ;;
    msmarco_passage|voxceleb2_ecapa_150k) echo 96 ;;
    *) return 2 ;;
  esac
}

coarse_cap() {
  case "$1" in
    amazon_esci) echo 6144 ;;
    coco_100k) echo 3072 ;;
    msmarco_passage) echo 12288 ;;
    voxceleb2_ecapa_150k) echo 1536 ;;
    *) return 2 ;;
  esac
}

rho_num() { [[ "$RHO" == */* ]] && echo "${RHO%%/*}" || echo "$RHO"; }
rho_den() { [[ "$RHO" == */* ]] && echo "${RHO##*/}" || echo 1; }

validate_result() {
  local result_file=$1 variant=$2
  local expected_mode=SE expected_rho_num expected_rho_den expected_layout=inline_hot_record_store
  expected_rho_num=$(rho_num)
  expected_rho_den=$(rho_den)
  if [[ "$variant" == NoCombine ]]; then
    expected_mode=GE
    expected_rho_num=0
    expected_rho_den=1
    expected_layout=no_combine_flatstor
  fi
  jq -e --arg expected_mode "$expected_mode" \
      --arg expected_layout "$expected_layout" \
      --argjson expected_rho_num "$expected_rho_num" \
      --argjson expected_rho_den "$expected_rho_den" '
    .metrics as $c | .pipeline_stats as $p |
    $c.vec_span_planner_mode == $expected_mode and
    $c.record_layout == $expected_layout and
    $c.vec_span_coalescing_enabled == true and
    $c.vec_span_alpha_num == 3 and $c.vec_span_alpha_den == 2 and
    $c.vec_span_safein_rho_num == $expected_rho_num and
    $c.vec_span_safein_rho_den == $expected_rho_den and
    $c.vec_span_safein_tail_count == 0 and
    $c.materialization_mode == "late" and
    $c.safein_as_vec_only == true and
    $p.avg_all_read_requests == 0 and
    $p.avg_vec_span_planner_fallbacks == 0 and
    $p.avg_vec_span_planned_physical_bytes == $p.avg_vec_only_read_bytes and
    $p.avg_vec_span_planner_groups == $p.avg_vec_only_read_requests and
    (if $expected_mode == "GE" then $p.avg_vec_span_planner_credit_bytes == 0
     else $p.avg_vec_span_planner_credit_bytes >= 0 end)
  ' "$result_file" >/dev/null || {
    echo "invalid SE/NoCombine result contract: $result_file" >&2
    return 3
  }
}

run_one() {
  local dataset=$1 variant=$2 output=$3 queries=$4 cache_mode=$5
  local store mode rho reuse=1 log
  local -a layout
  store=$(combined_store "$dataset")
  mode=SE
  rho=$RHO
  layout=(--inline-hot-record-store-dir "$store")
  if [[ "$variant" == NoCombine ]]; then
    mode=GE
    rho=0
    reuse=0
    layout=(--separate-store-dir "$NOCOMBINE_ROOT/$dataset")
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
    --queries "$queries" --topk 100 --nprobe "$(nprobe_for "$dataset")" \
    --index-dir "$store" "${layout[@]}" --output "$output" \
    --rabitq-validation-mode official_1_plus_n \
    --rabitq-active-bits 4 --rabitq-resident-bits 4 \
    --dynamic-safeout 1 --dynamic-safein frontier \
    --materialization-mode late --safein-as-vec-only 1 \
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
    --vec-span-planner-mode "$mode" --vec-span-alpha=3/2 \
    --vec-span-safein-rho="$rho" --vec-span-payload-reuse "$reuse" \
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
    [[ -s "$(combined_store "$dataset")/manifest.json" ]] || {
      echo "missing combined store for $dataset" >&2
      exit 2
    }
    [[ -s "$NOCOMBINE_ROOT/$dataset/manifest.json" ]] || {
      echo "missing NoCombine store for $dataset" >&2
      exit 2
    }
    if ((rep % 2)); then variants="CombinedSE NoCombine"; else variants="NoCombine CombinedSE"; fi
    for variant in $variants; do
      warm="$OUT_ROOT/rep$rep/warmup/$dataset/$variant"
      measured="$OUT_ROOT/rep$rep/runs/q$QUERIES/$dataset/$variant"
      if [[ ! -s "$measured/results.json" ]]; then
        [[ -s "$warm/results.json" ]] || \
          run_one "$dataset" "$variant" "$warm" "$WARMUP_QUERIES" default
        run_one "$dataset" "$variant" "$measured" "$QUERIES" drop-before-queries
      else
        validate_result "$measured/results.json" "$variant"
      fi
      echo "[done] rep=$rep dataset=$dataset variant=$variant"
    done
  done
done

echo "SE NoCombine results: $OUT_ROOT"
