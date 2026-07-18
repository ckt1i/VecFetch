#!/usr/bin/env bash
# Historical runner retained as non-runnable legacy evidence. Its candidate-
# budget command is intentionally incompatible with current benchmark binaries.
set -euo pipefail

REPO_ROOT="${REPO_ROOT:-/home/zcq/VDB/VectorRetrival}"
OUT_ROOT="${OUT_ROOT:-/home/zcq/VDB/test/dynamic_exbits_stage2_20260628}"
BENCH_BUILD="${BENCH_BUILD:-$REPO_ROOT/build/benchmarks/bench_build_index}"
BENCH_QUERY="${BENCH_QUERY:-$REPO_ROOT/build/benchmarks/bench_e2e}"
QUERIES="${QUERIES:-1000}"
REPS="${REPS:-1}"
RUN_LABEL="${RUN_LABEL:-stage2_format_formal}"

mkdir -p "$OUT_ROOT"/{indexes,runs,logs,results}

die() {
  echo "ERROR: $*" >&2
  exit 1
}

need_file() {
  [[ -f "$1" ]] || die "missing file: $1"
}

dataset_dir() {
  case "$1" in
    amazon_esci) echo "/home/zcq/VDB/data/bench_e2e/amazon_esci" ;;
    msmarco_passage) echo "/home/zcq/VDB/data/bench_e2e/msmarco_passage" ;;
    *) die "unknown dataset: $1" ;;
  esac
}

query_file() { echo "$(dataset_dir "$1")/query_embeddings.npy"; }
query_ids() { echo "$(dataset_dir "$1")/query_ids.npy"; }

gt_file() {
  case "$1" in
    amazon_esci) echo "/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/gt/gt_top100.npy" ;;
    msmarco_passage) echo "/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/gt/gt_top100.npy" ;;
    *) die "unknown dataset: $1" ;;
  esac
}

nlist_for() {
  case "$1" in
    amazon_esci) echo "8192" ;;
    msmarco_passage) echo "16384" ;;
    *) die "unknown dataset: $1" ;;
  esac
}

centroids_for() {
  case "$1" in
    amazon_esci) echo "/home/zcq/VDB/test/rabitq_fair_ex3_20260624/artifacts/amazon_esci/centroids.fvecs" ;;
    msmarco_passage) echo "/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings/msmarco_passage_centroid_16384.fvecs" ;;
    *) die "unknown dataset: $1" ;;
  esac
}

assignments_for() {
  case "$1" in
    amazon_esci) echo "/home/zcq/VDB/test/rabitq_fair_ex3_20260624/artifacts/amazon_esci/assignments.ivecs" ;;
    msmarco_passage) echo "/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings/msmarco_passage_cluster_id_16384.ivecs" ;;
    *) die "unknown dataset: $1" ;;
  esac
}

payload_index() {
  case "$1" in
    amazon_esci) echo "/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/payload_flatstor/default/index.npy" ;;
    msmarco_passage) echo "/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/payload_flatstor/default/index.npy" ;;
    *) die "unknown dataset: $1" ;;
  esac
}

payload_data() {
  case "$1" in
    amazon_esci) echo "/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/payload_flatstor/default/payload.dat" ;;
    msmarco_passage) echo "/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/payload_flatstor/default/payload.dat" ;;
    *) die "unknown dataset: $1" ;;
  esac
}

index_dir_for() {
  local ds="$1"
  local layout="$2"
  echo "$OUT_ROOT/indexes/$ds/${layout}_ex3/current_index_official_1_plus_n_total4_ex3_${layout}"
}

build_index() {
  local ds="$1"
  local layout="$2"
  local idx
  idx="$(index_dir_for "$ds" "$layout")"
  if [[ -f "$idx/cluster.clu" && -f "$idx/data.dat" && -f "$idx/segment.meta" ]]; then
    echo "[skip-build] $idx"
    return
  fi
  local out_base="$OUT_ROOT/indexes/$ds/${layout}_ex3"
  mkdir -p "$out_base"
  local log="$OUT_ROOT/logs/build_${ds}_${layout}_ex3.log"
  echo "[build] ds=$ds layout=$layout"
  "$BENCH_BUILD" \
    --dataset "$(dataset_dir "$ds")" \
    --output "$out_base" \
    --nlist "$(nlist_for "$ds")" \
    --nprobe 64 \
    --topk 10 \
    --queries 1000 \
    --skip-gt 1 \
    --rabitq-estimator-mode official_1_plus_n \
    --rabitq-total-bits 4 \
    --rabitq-ex-bits 3 \
    --rabitq-exdata-layout "$layout" \
    --centroids "$(centroids_for "$ds")" \
    --assignments "$(assignments_for "$ds")" \
    --epsilon-percentile 0.90 \
    --payload-mode raw-flatstor \
    --payload-index "$(payload_index "$ds")" \
    --payload-data "$(payload_data "$ds")" \
    >"$log" 2>&1
  local generated
  generated="$(find "$out_base" -mindepth 2 -maxdepth 2 -type d -name "index_official_1_plus_n_total4_ex3_${layout}" | sort | tail -n 1)"
  if [[ -z "$generated" || ! -f "$generated/cluster.clu" ]]; then
    die "missing generated index for ds=$ds layout=$layout; see $log"
  fi
  ln -sfn "$generated" "$idx"
  echo "[link] $idx -> $generated"
}

run_one() {
  local ds="$1"
  local layout="$2"
  local topk="$3"
  local nprobe="$4"
  local active="$5"
  local rep="$6"
  local idx
  idx="$(index_dir_for "$ds" "$layout")"
  need_file "$idx/cluster.clu"
  need_file "$idx/data.dat"
  need_file "$idx/segment.meta"

  local out="$OUT_ROOT/runs/$RUN_LABEL/$ds/$layout/active${active}/topk${topk}/np${nprobe}/rep${rep}"
  local result="$out/results.json"
  local log="$OUT_ROOT/logs/query_${ds}_${layout}_active${active}_topk${topk}_np${nprobe}_rep${rep}.log"
  mkdir -p "$out"
  if [[ -f "$result" ]]; then
    echo "[skip-query] $result"
    return
  fi
  echo "[query] ds=$ds layout=$layout active=$active topk=$topk nprobe=$nprobe rep=$rep"
  "$BENCH_QUERY" \
    --index-dir "$idx" \
    --query-file "$(query_file "$ds")" \
    --query-ids "$(query_ids "$ds")" \
    --gt-file "$(gt_file "$ds")" \
    --output "$out" \
    --topk "$topk" \
    --nprobe "$nprobe" \
    --queries "$QUERIES" \
    --nlist "$(nlist_for "$ds")" \
    --rabitq-estimator-mode official_1_plus_n \
    --rabitq-total-bits 4 \
    --rabitq-ex-bits 3 \
    --rabitq-active-ex-bits "$active" \
    --rabitq-exdata-layout "$layout" \
    --rabitq-validation-mode official_1_plus_n \
    --dynamic-safeout 1 \
    --dynamic-safein frontier \
    --dynamic-safein-stable-probes 1 \
    --dynamic-safein-rel-tol 0.005 \
    --dynamic-safein-defer-initial-clusters 4 \
    --dynamic-safein-defer-until-ready 1 \
    --non-safeout-candidate-budget 400 \
    --io-queue-depth 64 \
    --fixed-vec-buffer-count 1024 \
    --cluster-submit-reserve 8 \
    --submit-batch 32 \
    --two-level-coarse-routing 1 \
    --two-level-coarse-budget-factor 16 \
    --fine-grained-timing 0 \
    --hotpath-detailed-timing 0 \
    >"$log" 2>&1
}

need_file "$BENCH_BUILD"
need_file "$BENCH_QUERY"

DATASETS="${DATASETS:-amazon_esci msmarco_passage}"
LAYOUTS="${LAYOUTS:-vector_bitplanes tile_lane_bitmajor}"
TOPKS="${TOPKS:-10 100}"
NPROBES="${NPROBES:-64 128 256 512}"
ACTIVES="${ACTIVES:-3}"

for ds in $DATASETS; do
  need_file "$(query_file "$ds")"
  need_file "$(query_ids "$ds")"
  need_file "$(gt_file "$ds")"
  need_file "$(centroids_for "$ds")"
  need_file "$(assignments_for "$ds")"
  need_file "$(payload_index "$ds")"
  need_file "$(payload_data "$ds")"
  for layout in $LAYOUTS; do
    build_index "$ds" "$layout"
  done
done

for ds in $DATASETS; do
  for layout in $LAYOUTS; do
    for active in $ACTIVES; do
      for topk in $TOPKS; do
        for nprobe in $NPROBES; do
          for rep in $(seq 1 "$REPS"); do
            run_one "$ds" "$layout" "$topk" "$nprobe" "$active" "$rep"
          done
        done
      done
    done
  done
done

echo "[done] stage2 format formal sweep"
