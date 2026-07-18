#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/zcq/VDB}
REPO=${REPO:-$ROOT/VectorRetrival}
BINARY=${BINARY:-$REPO/build/benchmarks/bench_e2e}
OUT_ROOT=${OUT_ROOT:-$ROOT/test/recordgate_minimal_ablation_pilot_20260718}

DATASET=msmarco_passage
DATASET_DIR=$ROOT/data/bench_e2e/$DATASET
INPUT_ROOT=$ROOT/test/recordgate_safein_confidence_bextra_20260712/inputs/$DATASET/holdout
ADAPTIVE_STORE=$ROOT/test/recordgate_vec_span_stage1_20260715/stores/$DATASET/compact_g4_prefix0_align16
SPLIT_STORE=$ROOT/test/recordgate_nocombine_safein_current_span_20260717/stores/$DATASET
PROFILE=$ROOT/test/recordgate_rabitq_pcve_runtime_20260718/profiles/np64_k10_${DATASET}_a4r4_pcve.json

TOPK=${TOPK:-10}
NPROBE=${NPROBE:-64}
SMOKE_QUERIES=${SMOKE_QUERIES:-20}
FORMAL_QUERIES=${FORMAL_QUERIES:-200}
REPEATS=${REPEATS:-3}
CPU=${CPU:-8}
PHASE=${PHASE:-all}

CONFIGS=(
  verifyall_full
  pcve_nospan
  pcve_gv_noreuse
  pcve_gv_reuse
  pcve_split_gv
)

die() {
  echo "error: $*" >&2
  exit 1
}

config_values() {
  case "$1" in
    verifyall_full)    echo "0 1 1 adaptive" ;;
    pcve_nospan)       echo "1 0 0 adaptive" ;;
    pcve_gv_noreuse)   echo "1 1 0 adaptive" ;;
    pcve_gv_reuse)     echo "1 1 1 adaptive" ;;
    pcve_split_gv)     echo "1 1 0 split" ;;
    *) die "unknown config: $1" ;;
  esac
}

validate_inputs() {
  [[ -x "$BINARY" ]] || die "missing binary: $BINARY"
  command -v jq >/dev/null || die "jq is required"
  command -v taskset >/dev/null || die "taskset is required"
  [[ -s "$PROFILE" ]] || die "missing profile: $PROFILE"
  [[ -s "$ADAPTIVE_STORE/manifest.json" ]] || die "missing adaptive store"
  [[ -s "$SPLIT_STORE/manifest.json" ]] || die "missing split store"
  [[ -s "$INPUT_ROOT/query_embeddings.npy" ]] || die "missing query file"
  [[ -s "$INPUT_ROOT/query_ids.npy" ]] || die "missing query ids"
  [[ -s "$INPUT_ROOT/gt_top100.npy" ]] || die "missing GT"

  jq -e --arg dataset "$DATASET_DIR" --arg index "$ADAPTIVE_STORE" \
    --argjson nprobe "$NPROBE" --argjson topk "$TOPK" '
      .schema_version == 2 and
      .dataset_dir == $dataset and
      .index_dir == $index and
      .method == "pcve" and
      .stored_bits == 4 and .active_bits == 4 and .resident_bits == 4 and
      .nprobe == $nprobe and .topk == $topk
    ' "$PROFILE" >/dev/null || die "profile binding mismatch"
}

validate_result() {
  local result=$1 config=$2
  local safeout span reuse layout
  read -r safeout span reuse layout <<<"$(config_values "$config")"

  jq -e \
    --arg config "$config" \
    --arg split "$SPLIT_STORE" \
    --argjson safeout "$safeout" \
    --argjson span "$span" \
    --argjson reuse "$reuse" \
    --argjson topk "$TOPK" \
    --argjson nprobe "$NPROBE" '
      .metrics as $m |
      .pipeline_stats as $p |
      $m.topk == $topk and $m.nprobe == $nprobe and
      $m.rabitq_active_bits == 4 and $m.rabitq_resident_loaded_bits == 4 and
      $m.rabitq_pcve_requested == true and
      $m.rabitq_pcve_profile_valid == true and
      $m.rabitq_pcve_verify_all_fallback == false and
      $m.dynamic_safeout_enabled == ($safeout == 1) and
      $m.dynamic_safein_mode == "static" and
      $m.materialization_mode == "late" and
      $m.safein_as_vec_only == true and
      $m.execution_mode == "overlap" and
      $m.vec_span_coalescing_enabled == ($span == 1) and
      $m.vec_span_payload_reuse_enabled == ($reuse == 1) and
      $m.vec_span_payload_compact_enabled == false and
      $m.vec_span_safein_tail_count == 0 and
      $m.vec_span_safein_rho_num == 0 and
      $m.vec_span_safein_rho_den == 1 and
      $m.vec_span_planner_mode == "GV" and
      $m.vec_span_alpha_num == 3 and $m.vec_span_alpha_den == 2 and
      (($config == "pcve_split_gv" and $m.separate_store_dir == $split) or
       ($config != "pcve_split_gv" and $m.separate_store_dir == "")) and
      $p.avg_all_read_requests == 0 and
      $p.avg_safein_prefetch_candidates == 0 and
      $p.avg_vec_span_safein_tails_extended == 0 and
      $p.avg_vec_span_planner_fallbacks == 0
    ' "$result" >/dev/null || die "result validation failed: $result"
}

run_one() {
  local config=$1 label=$2 queries=$3 output=$4
  local safeout span reuse layout log
  local -a layout_args
  read -r safeout span reuse layout <<<"$(config_values "$config")"

  if [[ "$layout" == adaptive ]]; then
    layout_args=(--inline-hot-record-store-dir "$ADAPTIVE_STORE")
  else
    layout_args=(--separate-store-dir "$SPLIT_STORE")
  fi

  log=$OUT_ROOT/logs/${config}_${label}.log
  if [[ -s "$output/results.json" ]]; then
    validate_result "$output/results.json" "$config"
    echo "[skip] $config $label"
    return
  fi

  mkdir -p "$output" "$(dirname "$log")"
  echo "[run] $config $label queries=$queries"
  VDB_RESIDENT_HUGEPAGE=0 taskset -c "$CPU" "$BINARY" \
    --dataset "$DATASET_DIR" \
    --query-file "$INPUT_ROOT/query_embeddings.npy" \
    --query-ids "$INPUT_ROOT/query_ids.npy" \
    --gt-file "$INPUT_ROOT/gt_top100.npy" \
    --query-offset 0 --gt-offset 0 --queries "$queries" \
    --index-dir "$ADAPTIVE_STORE" "${layout_args[@]}" \
    --output "$output" --topk "$TOPK" --nprobe "$NPROBE" \
    --rabitq-validation-mode official_1_plus_n \
    --rabitq-active-bits 4 --rabitq-resident-bits 4 \
    --rabitq-pcve-profile "$PROFILE" --rabitq-pcve-fallback fail \
    --dynamic-safeout "$safeout" \
    --dynamic-safein static --enable-stage1-safein 0 \
    --materialization-mode late --safein-as-vec-only 1 \
    --safein-prefetch-order arrival --skip-false-stats 1 \
    --payload-cache-mode default \
    --io-queue-depth 64 --fixed-vec-buffer-count 1024 \
    --cluster-submit-reserve 8 --submit-batch 32 \
    --submission-mode shared --sqpoll 0 --iopoll 0 \
    --execution-mode overlap \
    --vec-span-coalescing "$span" --vec-span-tile-bytes 65536 \
    --vec-span-planner-mode GV --vec-span-alpha=3/2 \
    --vec-span-safein-rho=0 --vec-span-payload-reuse "$reuse" \
    --vec-span-payload-compact 0 --vec-span-safein-tail-count 0 \
    --two-level-coarse-routing 1 --two-level-coarse-threshold 4096 \
    --two-level-coarse-budget-factor 16 --two-level-coarse-budget-cap 12288 \
    >"$log" 2>&1

  validate_result "$output/results.json" "$config"
}

run_smoke() {
  for config in "${CONFIGS[@]}"; do
    run_one "$config" smoke "$SMOKE_QUERIES" \
      "$OUT_ROOT/smoke/$DATASET/$config"
  done
}

formal_order() {
  case "$1" in
    1) echo "verifyall_full pcve_nospan pcve_gv_noreuse pcve_gv_reuse pcve_split_gv" ;;
    2) echo "pcve_split_gv pcve_gv_reuse pcve_gv_noreuse pcve_nospan verifyall_full" ;;
    *) echo "pcve_gv_noreuse pcve_gv_reuse pcve_split_gv verifyall_full pcve_nospan" ;;
  esac
}

run_formal() {
  local config rep
  for rep in $(seq 1 "$REPEATS"); do
    for config in $(formal_order "$rep"); do
      run_one "$config" "rep$rep" "$FORMAL_QUERIES" \
        "$OUT_ROOT/runs/$DATASET/np${NPROBE}_k${TOPK}/$config/rep$rep"
    done
  done
}

write_summary() {
  local summary=$OUT_ROOT/formal_summary.tsv
  local config rep result
  printf '%s\n' \
    $'dataset\tconfig\trep\tqueries\trecall_at_10\tavg_ms\tp95_ms\tp99_ms\tqps\ttotal_probed\tsafeout\treranked\tvec_requests\tvec_bytes\tspan_requests\tspan_candidates\tspan_bytes\tplanned_physical_bytes\tplanned_vector_bytes\trealized_amplification\tpayload_requests\tpayload_bytes\treuse_hits\treuse_bytes\trequests_avoided\ttotal_read_bytes\ttotal_io_submitted\tplanner_ms\tsort_ms\tplanner_fallbacks\tpeak_rss_kib' \
    >"$summary"

  for config in "${CONFIGS[@]}"; do
    for rep in $(seq 1 "$REPEATS"); do
      result=$OUT_ROOT/runs/$DATASET/np${NPROBE}_k${TOPK}/$config/rep$rep/results.json
      [[ -s "$result" ]] || continue
      jq -r --arg dataset "$DATASET" --arg config "$config" --arg rep "$rep" '
        .metrics as $m | .pipeline_stats as $p |
        ($p.avg_vec_span_planned_physical_bytes /
          (if $p.avg_vec_span_planned_vector_bytes > 0
           then $p.avg_vec_span_planned_vector_bytes else 1 end)) as $amp |
        [
          $dataset, $config, $rep, $m.num_queries,
          $m.recall_at_10, $m.avg_query_time_ms, $m.p95_ms, $m.p99_ms,
          (1000.0 / $m.avg_query_time_ms),
          $p.avg_total_probed, $p.avg_safe_out, $p.avg_candidates_reranked,
          $p.avg_vec_only_read_requests, $p.avg_vec_only_read_bytes,
          $p.avg_vec_span_read_requests, $p.avg_vec_span_candidates,
          $p.avg_vec_span_read_bytes,
          $p.avg_vec_span_planned_physical_bytes,
          $p.avg_vec_span_planned_vector_bytes, $amp,
          $p.avg_payload_read_requests, $p.avg_payload_read_bytes,
          $p.avg_vec_span_payload_reuse_hits,
          $p.avg_vec_span_payload_reuse_bytes,
          $p.avg_vec_span_payload_requests_avoided,
          $p.avg_total_read_bytes, $p.avg_total_io_submitted,
          $p.avg_vec_span_planner_ms, $p.avg_vec_span_sort_ms,
          $p.avg_vec_span_planner_fallbacks,
          .rss_profile.peak_during_queries_rss_kib
        ] | @tsv
      ' "$result" >>"$summary"
    done
  done
  echo "summary: $summary"
}

validate_inputs
mkdir -p "$OUT_ROOT"

case "$PHASE" in
  smoke) run_smoke ;;
  formal) run_formal; write_summary ;;
  all) run_smoke; run_formal; write_summary ;;
  *) die "PHASE must be smoke, formal, or all" ;;
esac

echo "results: $OUT_ROOT"
