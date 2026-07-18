#!/usr/bin/env python3
"""Run the Amazon/MSMARCO No Combine ablation sweep.

The script intentionally reuses existing indexes and materialized separate
stores. It only writes result directories and CSV summaries under /home/zcq/VDB/test.
"""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
import time
from pathlib import Path


REPO = Path("/home/zcq/VDB/VectorRetrival")
BENCH = REPO / "build/benchmarks/bench_e2e"
ROOT = Path("/home/zcq/VDB/test/no_combine_ablation_amazon_msmarco_ex3_20260627")

DATASETS = {
    "amazon_esci": {
        "index": Path("/home/zcq/VDB/test/rabitq_fair_ex3_20260624/indexes/amazon_esci/official_vector_bitplanes/current_index_official_1_plus_n_total4_ex3_vector_bitplanes"),
        "store": ROOT / "stores/amazon_esci/official_vector_bitplanes",
        "query": Path("/home/zcq/VDB/data/bench_e2e/amazon_esci/query_embeddings.npy"),
        "query_ids": Path("/home/zcq/VDB/data/bench_e2e/amazon_esci/query_ids.npy"),
        "gt": Path("/home/zcq/VDB/baselines/data/formal_baselines/amazon_esci/gt/gt_top100.npy"),
    },
    "msmarco_passage": {
        "index": Path("/home/zcq/VDB/test/rabitq_fair_ex3_20260624/indexes/msmarco_passage/official_vector_bitplanes/current_index_official_1_plus_n_total4_ex3_vector_bitplanes"),
        "store": ROOT / "stores/msmarco_passage/official_vector_bitplanes",
        "query": Path("/home/zcq/VDB/data/bench_e2e/msmarco_passage/query_embeddings.npy"),
        "query_ids": Path("/home/zcq/VDB/data/bench_e2e/msmarco_passage/query_ids.npy"),
        "gt": Path("/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/gt/gt_top100.npy"),
    },
}

LAYOUTS = ("full", "no_combine_flatstor")
TOPKS = (10, 100)
NPROBES = (64, 128, 256, 512)


def build_cmd(dataset: str, layout: str, topk: int, nprobe: int, queries: int) -> tuple[list[str], Path]:
    cfg = DATASETS[dataset]
    out_dir = ROOT / "formal" / dataset / layout / f"k{topk}_np{nprobe}"
    cmd = [
        str(BENCH),
        "--index-dir", str(cfg["index"]),
        "--query-file", str(cfg["query"]),
        "--query-ids", str(cfg["query_ids"]),
        "--gt-file", str(cfg["gt"]),
        "--output", str(out_dir),
        "--queries", str(queries),
        "--topk", str(topk),
        "--nprobe", str(nprobe),
        "--rabitq-validation-mode", "official_1_plus_n",
        "--dynamic-safeout", "1",
        "--dynamic-safein", "frontier",
        "--dynamic-safein-stable-probes", "1",
        "--dynamic-safein-rel-tol", "0.005",
        "--dynamic-safein-defer-initial-clusters", "4",
        "--dynamic-safein-defer-until-ready", "1",
        "--io-queue-depth", "64",
        "--fixed-vec-buffer-count", "1024",
        "--cluster-submit-reserve", "8",
        "--submit-batch", "32",
        "--fine-grained-timing", "0",
        "--hotpath-detailed-timing", "0",
        "--two-level-coarse-routing", "1",
        "--two-level-coarse-budget-factor", "16",
    ]
    if layout == "no_combine_flatstor":
        cmd[1:1] = ["--separate-store-dir", str(cfg["store"])]
    return cmd, out_dir


def flatten_result(dataset: str, layout: str, topk: int, nprobe: int, out_dir: Path) -> dict[str, object]:
    path = out_dir / "results.json"
    d = json.loads(path.read_text())
    m = d["metrics"]
    p = d["pipeline_stats"]
    r = d["rss_profile"]
    return {
        "dataset": dataset,
        "layout": layout,
        "topk": topk,
        "nprobe": nprobe,
        "queries": m["num_queries"],
        "recall_at_10": m.get("recall_at_10", 0.0),
        "recall_at_k": m.get("recall_at_k", 0.0),
        "avg_ms": m["avg_query_time_ms"],
        "p50_ms": m["p50_ms"],
        "p95_ms": m["p95_ms"],
        "p99_ms": m["p99_ms"],
        "qps": 1000.0 / m["avg_query_time_ms"] if m["avg_query_time_ms"] > 0 else 0.0,
        "avg_total_probed": p["avg_total_probed"],
        "avg_safe_in": p["avg_safe_in"],
        "avg_safe_out": p["avg_safe_out"],
        "avg_uncertain": p["avg_uncertain"],
        "avg_candidates_reranked": p["avg_candidates_reranked"],
        "avg_vec_only_read_requests": p["avg_vec_only_read_requests"],
        "avg_all_read_requests": p["avg_all_read_requests"],
        "avg_payload_read_requests": p["avg_payload_read_requests"],
        "avg_vec_only_read_bytes": p.get("avg_vec_only_read_bytes", 0.0),
        "avg_all_read_bytes": p.get("avg_all_read_bytes", 0.0),
        "avg_payload_read_bytes": p.get("avg_payload_read_bytes", 0.0),
        "avg_total_read_bytes": p.get("avg_total_read_bytes", 0.0),
        "avg_separate_store_lookup_misses": p.get("avg_separate_store_lookup_misses", 0.0),
        "after_preload_rss_kib": r["after_preload_rss_kib"],
        "peak_during_queries_rss_kib": r["peak_during_queries_rss_kib"],
        "query_peak_delta_kib": r["query_peak_delta_kib"],
        "results_json": str(path),
    }


def write_summary(rows: list[dict[str, object]]) -> None:
    out = ROOT / "formal_summary.csv"
    if not rows:
        return
    with out.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)
    print(f"[summary] {out}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--queries", type=int, default=1000)
    ap.add_argument("--rerun", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    rows: list[dict[str, object]] = []
    total = len(DATASETS) * len(LAYOUTS) * len(TOPKS) * len(NPROBES)
    done = 0
    for dataset in DATASETS:
        for topk in TOPKS:
            for nprobe in NPROBES:
                for layout in LAYOUTS:
                    done += 1
                    cmd, out_dir = build_cmd(dataset, layout, topk, nprobe, args.queries)
                    result_json = out_dir / "results.json"
                    print(f"[{done}/{total}] {dataset} {layout} topk={topk} nprobe={nprobe}")
                    if args.dry_run:
                        print(" ".join(cmd))
                        continue
                    if result_json.exists() and not args.rerun:
                        print(f"  skip existing: {result_json}")
                        rows.append(flatten_result(dataset, layout, topk, nprobe, out_dir))
                        continue
                    out_dir.mkdir(parents=True, exist_ok=True)
                    t0 = time.time()
                    proc = subprocess.run(cmd, cwd=REPO, text=True,
                                          stdout=subprocess.PIPE,
                                          stderr=subprocess.STDOUT)
                    (out_dir / "run.log").write_text(proc.stdout)
                    elapsed = time.time() - t0
                    print(f"  exit={proc.returncode} wall={elapsed:.2f}s")
                    if proc.returncode != 0:
                        print(proc.stdout[-4000:], file=sys.stderr)
                        return proc.returncode
                    rows.append(flatten_result(dataset, layout, topk, nprobe, out_dir))
                    write_summary(rows)

    write_summary(rows)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
