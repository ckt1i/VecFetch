#!/usr/bin/env bash
set -euo pipefail

ROOT=${ROOT:-/home/zcq/VDB}
REPO=${REPO:-$ROOT/VectorRetrival}
BINARY=${BINARY:-$REPO/build/benchmarks/bench_e2e}
INPUT_ROOT=${INPUT_ROOT:-$ROOT/test/recordgate_safein_confidence_bextra_20260712/inputs}
STORE_ROOT=${STORE_ROOT:-$ROOT/test/recordgate_vec_span_stage1_20260715/stores}
OUT_ROOT=${OUT_ROOT:-$ROOT/test/recordgate_rabitq_pcve_runtime_20260718}
DATASETS=${DATASETS:-"amazon_esci msmarco_passage"}
CONFIGS=${CONFIGS:-"a4r4_pcve a3r4_pcve a3r4_apcve a3r3_apcve"}
NPROBE=${NPROBE:-64}
TOPK=${TOPK:-10}
RUN_TAG=${RUN_TAG:-np${NPROBE}_k${TOPK}}
FORMAL_QUERIES=${FORMAL_QUERIES:-500}
WARMUP_QUERIES=${WARMUP_QUERIES:-20}
REPEATS=${REPEATS:-3}
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
  esac
}

config_bits() {
  case "$1" in
    a4r4_pcve) echo "4 4 pcve" ;;
    a3r4_pcve) echo "3 4 pcve" ;;
    a3r4_apcve) echo "3 4 a_pcve" ;;
    a3r3_apcve) echo "3 3 a_pcve" ;;
    *) echo "unknown config: $1" >&2; return 2 ;;
  esac
}

profile_values() {
  local dataset=$1 config=$2
  case "$dataset/$config" in
    amazon_esci/a4r4_pcve) echo "0.1625217944 0.1625217944 0.01944966428 0.01944966428" ;;
    amazon_esci/a3r4_pcve|amazon_esci/a3r3_pcve) echo "0.1625217944 0.1625217944 0.176349476 0.176349476" ;;
    amazon_esci/a3r4_apcve|amazon_esci/a3r3_apcve) echo "0.1625217944 0.1249603629 0.1589672118 0.176349476" ;;
    msmarco_passage/a4r4_pcve) echo "0.1772570163 0.1772570163 0.02234215103 0.02234215103" ;;
    msmarco_passage/a3r4_pcve|msmarco_passage/a3r3_pcve) echo "0.1772570163 0.1772570163 0.1923076808 0.1923076808" ;;
    msmarco_passage/a3r4_apcve|msmarco_passage/a3r3_apcve) echo "0.1778161079 0.1455557048 0.1589547396 0.194512561" ;;
    *) echo "missing profile values: $dataset/$config" >&2; return 2 ;;
  esac
}

write_profile() {
  local dataset=$1 config=$2 profile=$3
  local active resident method s1l s1u s2l s2u store dataset_dir
  read -r active resident method <<<"$(config_bits "$config")"
  read -r s1l s1u s2l s2u <<<"$(profile_values "$dataset" "$config")"
  store=$(store_for "$dataset")
  dataset_dir="$ROOT/data/bench_e2e/$dataset"
  mkdir -p "$(dirname "$profile")"
  cat >"$profile" <<JSON
{
  "schema_version": 2,
  "dataset_dir": "$dataset_dir",
  "index_dir": "$store",
  "estimator_mode": "official_1_plus_n",
  "code_layout": "tile_lane_bitmajor",
  "method": "$method",
  "stored_bits": 4,
  "active_bits": $active,
  "resident_bits": $resident,
  "nprobe": $NPROBE,
  "topk": $TOPK,
  "stage1_epsilon_lower": $s1l,
  "stage1_epsilon_upper": $s1u,
  "stage2_epsilon_lower": $s2l,
  "stage2_epsilon_upper": $s2u
}
JSON
}

validate_result() {
  local result=$1 active=$2 resident=$3
  jq -e --argjson active "$active" --argjson resident "$resident" --argjson topk "$TOPK" '
    .metrics as $m |
    $m.rabitq_pcve_requested == true and
    $m.rabitq_pcve_profile_valid == true and
    $m.rabitq_pcve_verify_all_fallback == false and
    $m.rabitq_active_bits == $active and
    $m.rabitq_resident_loaded_bits == $resident and
    $m.topk == $topk and
    $m.dynamic_safein_mode == "static" and
    $m.vec_span_planner_mode == "GV" and
    $m.vec_span_coalescing_enabled == true
  ' "$result" >/dev/null
}

run_one() {
  local dataset=$1 config=$2 label=$3 queries=$4 output=$5
  local active resident method profile store log
  read -r active resident method <<<"$(config_bits "$config")"
  profile="$OUT_ROOT/profiles/${RUN_TAG}_${dataset}_${config}.json"
  write_profile "$dataset" "$config" "$profile"
  store=$(store_for "$dataset")
  log="$OUT_ROOT/logs/${RUN_TAG}/${dataset}_${config}_${label}.log"
  if [[ -s "$output/results.json" ]]; then
    validate_result "$output/results.json" "$active" "$resident"
    echo "[skip] $dataset $config $label"
    return
  fi
  mkdir -p "$output" "$(dirname "$log")"
  echo "[run] $dataset $config $label"
  VDB_RESIDENT_HUGEPAGE=0 taskset -c "$CPU" "$BINARY" \
    --dataset "$ROOT/data/bench_e2e/$dataset" \
    --query-file "$INPUT_ROOT/$dataset/holdout/query_embeddings.npy" \
    --query-ids "$INPUT_ROOT/$dataset/holdout/query_ids.npy" \
    --gt-file "$INPUT_ROOT/$dataset/holdout/gt_top100.npy" \
    --query-offset 0 --gt-offset 0 --queries "$queries" \
    --index-dir "$store" --inline-hot-record-store-dir "$store" \
    --output "$output" --topk "$TOPK" --nprobe "$NPROBE" \
    --rabitq-validation-mode official_1_plus_n \
    --rabitq-active-bits "$active" --rabitq-resident-bits "$resident" \
    --rabitq-pcve-profile "$profile" --rabitq-pcve-fallback fail \
    --dynamic-safeout 1 --dynamic-safein static --enable-stage1-safein 0 \
    --materialization-mode late --safein-as-vec-only 1 \
    --safein-prefetch-order arrival --skip-false-stats 1 \
    --io-queue-depth 64 --fixed-vec-buffer-count 1024 \
    --cluster-submit-reserve 8 --submit-batch 32 \
    --submission-mode shared --sqpoll 0 --iopoll 0 \
    --vec-span-coalescing 1 --vec-span-tile-bytes 65536 \
    --vec-span-planner-mode GV --vec-span-alpha=3/2 \
    --vec-span-safein-rho=0 --vec-span-payload-reuse 1 \
    --vec-span-payload-compact 0 --vec-span-safein-tail-count 0 \
    --two-level-coarse-routing 1 --two-level-coarse-threshold 4096 \
    --two-level-coarse-budget-factor 16 \
    --two-level-coarse-budget-cap "$(coarse_cap "$dataset")" \
    >"$log" 2>&1
  validate_result "$output/results.json" "$active" "$resident"
}

[[ -x "$BINARY" ]] || { echo "missing binary: $BINARY" >&2; exit 1; }
command -v jq >/dev/null || { echo "jq is required" >&2; exit 1; }
mkdir -p "$OUT_ROOT"
for dataset in $DATASETS; do
  [[ -s "$(store_for "$dataset")/manifest.json" ]] || {
    echo "missing store: $(store_for "$dataset")" >&2; exit 1;
  }
  for config in $CONFIGS; do
    run_one "$dataset" "$config" warmup "$WARMUP_QUERIES" \
      "$OUT_ROOT/runs/$RUN_TAG/$dataset/$config/warmup"
    for rep in $(seq 1 "$REPEATS"); do
      run_one "$dataset" "$config" "rep$rep" "$FORMAL_QUERIES" \
        "$OUT_ROOT/runs/$RUN_TAG/$dataset/$config/rep$rep"
    done
  done
done

summary="$OUT_ROOT/formal_summary_${RUN_TAG}.tsv"
printf "dataset\tconfig\trep\trecall_at_k\tavg_ms\tp95_ms\tp99_ms\tvec_requests\tvec_bytes\treranked\tpeak_rss_kib\n" >"$summary"
for dataset in $DATASETS; do
  for config in $CONFIGS; do
    for rep in $(seq 1 "$REPEATS"); do
      result="$OUT_ROOT/runs/$RUN_TAG/$dataset/$config/rep$rep/results.json"
      jq -r --arg dataset "$dataset" --arg config "$config" --arg rep "$rep" '
        [
          $dataset, $config, $rep,
          .metrics.recall_at_k, .metrics.avg_query_time_ms,
          .metrics.p95_ms, .metrics.p99_ms,
          .pipeline_stats.avg_vec_only_read_requests,
          .pipeline_stats.avg_vec_only_read_bytes,
          .pipeline_stats.avg_candidates_reranked,
          .rss_profile.peak_during_queries_rss_kib
        ] | @tsv
      ' "$result" >>"$summary"
    done
  done
done

echo "A-PCVE runtime results: $OUT_ROOT/runs/$RUN_TAG"
echo "Formal summary: $summary"
