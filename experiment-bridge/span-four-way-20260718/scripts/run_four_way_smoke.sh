#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/zcq/VDB}
REPO=${REPO:-$ROOT/VectorRetrival}
BINARY=${BINARY:-$REPO/build/benchmarks/bench_e2e}
INPUT_ROOT=${INPUT_ROOT:-$ROOT/test/recordgate_safein_confidence_bextra_20260712/inputs}
EPS_ROOT=${EPS_ROOT:-$ROOT/test/recordgate_hybrid_split_static_safein_20260712/runtime_epsilon_cache}
STORE_ROOT=${STORE_ROOT:-$ROOT/test/recordgate_vec_span_stage1_20260715/stores}
OUT_ROOT=${OUT_ROOT:-$ROOT/test/recordgate_span_four_way_smoke_20260718}
DATASETS=${DATASETS:-"msmarco_passage amazon_esci"}
QUERY_COUNTS=${QUERY_COUNTS:-"20 100"}
NPROBE=${NPROBE:-192}
CPU=${CPU:-8}
MODES=${MODES:-"GV SV GE SE"}
SAFEIN_RHO=${SAFEIN_RHO:-1}
COALESCING=${COALESCING:-1}

store_for() {
  case "$1" in
    msmarco_passage) echo "$STORE_ROOT/$1/compact_g4_prefix0_align16" ;;
    amazon_esci) echo "$STORE_ROOT/$1/compact_g4_prefix0_align8" ;;
    voxceleb2_ecapa_150k) echo "$STORE_ROOT/$1/compact_g4_prefix0_align8" ;;
    *) echo "unknown dataset: $1" >&2; return 2 ;;
  esac
}

coarse_cap() {
  case "$1" in
    msmarco_passage) echo 12288 ;;
    amazon_esci) echo 6144 ;;
    voxceleb2_ecapa_150k) echo 1536 ;;
    *) return 2 ;;
  esac
}

rho_for() {
  case "$1" in
    SV|SE) echo "$SAFEIN_RHO" ;;
    GV|GE) echo 0 ;;
  esac
}

validate_result() {
  local result_file=$1 mode=$2 dataset=$3
  local safein_mode=false require_positive_credit=false
  local expected_rho_num=0 expected_rho_den=1 expected_coalescing=false
  case "$COALESCING" in
    0) expected_coalescing=false ;;
    1) expected_coalescing=true ;;
    *) echo "COALESCING must be 0 or 1" >&2; return 2 ;;
  esac
  case "$mode" in
    SV|SE)
      safein_mode=true
      case "$SAFEIN_RHO" in
        0) expected_rho_num=0; expected_rho_den=1 ;;
        1/2) expected_rho_num=1; expected_rho_den=2 ;;
        1) expected_rho_num=1; expected_rho_den=1 ;;
        *) echo "unsupported SAFEIN_RHO: $SAFEIN_RHO" >&2; return 2 ;;
      esac
      if [[ "$COALESCING" == 1 && "$SAFEIN_RHO" != 0 &&
            "$dataset" != voxceleb2_ecapa_150k ]]; then
        require_positive_credit=true
      fi
      ;;
  esac
  if ! jq -e --arg mode "$mode" --argjson safein_mode "$safein_mode" \
      --argjson require_positive_credit "$require_positive_credit" \
      --argjson expected_rho_num "$expected_rho_num" \
      --argjson expected_rho_den "$expected_rho_den" \
      --argjson expected_coalescing "$expected_coalescing" '
      .metrics as $c | .pipeline_stats as $p |
      $c.vec_span_planner_mode == $mode and
      $c.vec_span_coalescing_enabled == $expected_coalescing and
      $c.vec_span_alpha_num == 3 and
      $c.vec_span_alpha_den == 2 and
      $c.vec_span_safein_rho_num == $expected_rho_num and
      $c.vec_span_safein_rho_den == $expected_rho_den and
      $c.vec_span_safein_tail_count == 0 and
      $p.avg_vec_span_planner_fallbacks == 0 and
      (if $expected_coalescing
       then
         $p.avg_vec_span_planned_physical_bytes ==
           $p.avg_vec_only_read_bytes and
         $p.avg_vec_span_planner_groups ==
           $p.avg_vec_only_read_requests and
         (if $safein_mode
          then (if $require_positive_credit
                then $p.avg_vec_span_planner_credit_bytes > 0
                else $p.avg_vec_span_planner_credit_bytes >= 0 end)
          else $p.avg_vec_span_planner_credit_bytes == 0 end)
       else
         $p.avg_vec_span_planner_calls == 0 and
         $p.avg_vec_span_planner_groups == 0 and
         $p.avg_vec_span_planned_physical_bytes == 0 and
         $p.avg_vec_span_planner_credit_bytes == 0
       end)
    ' "$result_file" >/dev/null; then
    echo "invalid planner result contract: $result_file" >&2
    return 3
  fi
}

run_one() {
  local dataset=$1 mode=$2 queries=$3
  local store output log
  store=$(store_for "$dataset")
  output="$OUT_ROOT/runs/q${queries}/$dataset/$mode"
  log="$OUT_ROOT/logs/q${queries}_${dataset}_${mode}.log"
  if [[ -s "$output/results.json" ]]; then
    validate_result "$output/results.json" "$mode" "$dataset"
    echo "[skip] q=$queries dataset=$dataset mode=$mode"
    return
  fi
  mkdir -p "$output" "$(dirname "$log")"
  echo "[run] q=$queries dataset=$dataset mode=$mode"
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
    --vec-span-coalescing "$COALESCING" --vec-span-tile-bytes 65536 \
    --vec-span-planner-mode="$mode" --vec-span-alpha=3/2 \
    --vec-span-safein-rho="$(rho_for "$mode")" \
    --vec-span-payload-reuse 1 --vec-span-payload-compact 0 \
    --vec-span-safein-tail-count 0 \
    --two-level-coarse-routing 1 --two-level-coarse-threshold 4096 \
    --two-level-coarse-budget-factor 16 \
    --two-level-coarse-budget-cap "$(coarse_cap "$dataset")" \
    >"$log" 2>&1
  validate_result "$output/results.json" "$mode" "$dataset"
}

[[ -x "$BINARY" ]] || { echo "missing binary: $BINARY" >&2; exit 1; }
for dataset in $DATASETS; do
  [[ -s "$(store_for "$dataset")/manifest.json" ]] || {
    echo "missing inline store for $dataset" >&2
    exit 1
  }
  for queries in $QUERY_COUNTS; do
    for mode in $MODES; do
      run_one "$dataset" "$mode" "$queries"
    done
  done
done

echo "Smoke results: $OUT_ROOT/runs"
