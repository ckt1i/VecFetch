#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/zcq/VDB}
REPO=${REPO:-$ROOT/VectorRetrival}
BINARY=${BINARY:-/tmp/vdb-span-four-way-build/benchmarks/bench_e2e}
INPUT_ROOT=${INPUT_ROOT:-$ROOT/test/recordgate_safein_confidence_bextra_20260712/inputs}
EPS_ROOT=${EPS_ROOT:-$ROOT/test/recordgate_hybrid_split_static_safein_20260712/runtime_epsilon_cache}
STORE_ROOT=${STORE_ROOT:-$ROOT/test/recordgate_vec_span_stage1_20260715/stores}
OUT_ROOT=${OUT_ROOT:-$ROOT/test/recordgate_span_se_low_rho_sweep_20260718}
DATASETS=${DATASETS:-"amazon_esci msmarco_passage"}
RHO_VALUES=${RHO_VALUES:-"1/4 1/10"}
REPS=${REPS:-5}
QUERIES=${QUERIES:-500}
WARMUP_QUERIES=${WARMUP_QUERIES:-500}
NPROBE=${NPROBE:-96}
CPU=${CPU:-8}

store_for() {
  case "$1" in
    amazon_esci) echo "$STORE_ROOT/$1/compact_g4_prefix0_align8" ;;
    msmarco_passage) echo "$STORE_ROOT/$1/compact_g4_prefix0_align16" ;;
    *) echo "unknown dataset: $1" >&2; return 2 ;;
  esac
}

coarse_cap() {
  case "$1" in
    amazon_esci) echo 6144 ;;
    msmarco_passage) echo 12288 ;;
    *) return 2 ;;
  esac
}

rho_tag() { printf '%s' "${1//\//_}"; }

rho_num() {
  case "$1" in
    0) echo 0 ;;
    1) echo 1 ;;
    */*) echo "${1%%/*}" ;;
    *) return 2 ;;
  esac
}

rho_den() {
  case "$1" in
    0|1) echo 1 ;;
    */*) echo "${1##*/}" ;;
    *) return 2 ;;
  esac
}

validate_result() {
  local result_file=$1 mode=$2 rho=$3 dataset=$4
  local expected_num=0 expected_den=1 require_credit=false
  if [[ "$mode" == SE ]]; then
    expected_num=$(rho_num "$rho")
    expected_den=$(rho_den "$rho")
    require_credit=true
  fi
  jq -e --arg mode "$mode" \
      --argjson expected_num "$expected_num" \
      --argjson expected_den "$expected_den" \
      --argjson require_credit "$require_credit" '
    .metrics as $c | .pipeline_stats as $p |
    $c.vec_span_planner_mode == $mode and
    $c.vec_span_coalescing_enabled == true and
    $c.vec_span_alpha_num == 3 and $c.vec_span_alpha_den == 2 and
    $c.vec_span_safein_rho_num == $expected_num and
    $c.vec_span_safein_rho_den == $expected_den and
    $c.vec_span_safein_tail_count == 0 and
    $c.safein_as_vec_only == true and
    $p.avg_vec_span_planner_fallbacks == 0 and
    $p.avg_vec_span_planned_physical_bytes == $p.avg_vec_only_read_bytes and
    $p.avg_vec_span_planner_groups == $p.avg_vec_only_read_requests and
    (if $require_credit then $p.avg_vec_span_planner_credit_bytes > 0
     else $p.avg_vec_span_planner_credit_bytes == 0 end)
  ' "$result_file" >/dev/null || {
    echo "invalid low-rho result contract: $result_file" >&2
    return 3
  }
}

run_one() {
  local dataset=$1 mode=$2 rho=$3 output=$4 queries=$5
  local store effective_rho log
  store=$(store_for "$dataset")
  effective_rho=0
  [[ "$mode" == SE ]] && effective_rho=$rho
  log=${output%/}/run.log
  mkdir -p "$output"
  VDB_RESIDENT_HUGEPAGE=0 taskset -c "$CPU" "$BINARY" \
    --dataset "$ROOT/data/bench_e2e/$dataset" \
    --query-file "$INPUT_ROOT/$dataset/holdout/query_embeddings.npy" \
    --query-ids "$INPUT_ROOT/$dataset/holdout/query_ids.npy" \
    --gt-file "$INPUT_ROOT/$dataset/holdout/gt_top100.npy" \
    --query-offset 0 --gt-offset 0 --queries "$queries" \
    --index-dir "$store" --inline-hot-record-store-dir "$store" \
    --output "$output" --topk 100 --nprobe "$NPROBE" \
    --rabitq-validation-mode official_1_plus_n \
    --rabitq-active-bits 4 --rabitq-resident-bits 4 \
    --dynamic-safeout 1 --dynamic-safein frontier \
    --materialization-mode late --safein-as-vec-only 1 \
    --safein-prefetch-order arrival --skip-false-stats 1 \
    --safeout-epsilon-percentile 0.99 --epsilon-samples 100 \
    --epsilon-sampling-mode legacy_per_cluster \
    --safeout-epsilon-cache "$EPS_ROOT/${dataset}_safeout_p0p99_s100_legacy_per_cluster.txt" \
    --io-queue-depth 64 --fixed-vec-buffer-count 1024 \
    --cluster-submit-reserve 8 --submit-batch 32 \
    --submission-mode shared --sqpoll 0 --iopoll 0 \
    --vec-span-coalescing 1 --vec-span-tile-bytes 65536 \
    --vec-span-planner-mode="$mode" --vec-span-alpha=3/2 \
    --vec-span-safein-rho="$effective_rho" \
    --vec-span-payload-reuse 1 --vec-span-payload-compact 0 \
    --vec-span-safein-tail-count 0 \
    --two-level-coarse-routing 1 --two-level-coarse-threshold 4096 \
    --two-level-coarse-budget-factor 16 \
    --two-level-coarse-budget-cap "$(coarse_cap "$dataset")" \
    >"$log" 2>&1
  validate_result "$output/results.json" "$mode" "$rho" "$dataset"
}

[[ -x "$BINARY" ]] || { echo "missing binary: $BINARY" >&2; exit 1; }
mkdir -p "$OUT_ROOT"
for rep in $(seq 1 "$REPS"); do
  if ((rep % 2)); then rho_order=$RHO_VALUES; else rho_order=$(printf '%s\n' $RHO_VALUES | tac | xargs); fi
  for dataset in $DATASETS; do
    [[ -s "$(store_for "$dataset")/manifest.json" ]] || {
      echo "missing inline store for $dataset" >&2
      exit 2
    }
    for rho in $rho_order; do
      tag=$(rho_tag "$rho")
      if ((rep % 2)); then modes="GE SE"; else modes="SE GE"; fi
      for mode in $modes; do
        warm="$OUT_ROOT/rho_$tag/rep$rep/warmup/q$WARMUP_QUERIES/$dataset/$mode"
        measured="$OUT_ROOT/rho_$tag/rep$rep/runs/q$QUERIES/$dataset/$mode"
        if [[ ! -s "$measured/results.json" ]]; then
          [[ -s "$warm/results.json" ]] || run_one "$dataset" "$mode" "$rho" "$warm" "$WARMUP_QUERIES"
          run_one "$dataset" "$mode" "$rho" "$measured" "$QUERIES"
        else
          validate_result "$measured/results.json" "$mode" "$rho" "$dataset"
        fi
        echo "[done] rep=$rep dataset=$dataset rho=$rho mode=$mode"
      done
    done
  done
done

echo "Low-rho results: $OUT_ROOT"
