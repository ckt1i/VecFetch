#!/usr/bin/env python3
"""Run aggressive query-adaptive SafeIn experiments on COCO100k.

The target metrics are:
  safein_prefetch_topk_coverage > 0.20
  safein_prefetch_false_rate < 0.20
"""

from __future__ import annotations

import csv
import json
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent
RUN_ROOT = ROOT / "aggressive_dynamic_prefetch_runs"
BENCH = REPO / "build" / "benchmarks" / "bench_vector_search"

DATA = Path("/home/zcq/VDB/data/coco_100k")
BASE = DATA / "image_embeddings.npy"
QUERY = DATA / "query_embeddings.npy"
IMAGE_IDS = DATA / "image_ids.npy"
INDEX_DIR = Path("/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90")


SCHEMES = [
    {
        "name": "static_p090",
        "p": "0.90",
        "args": ["--dynamic-safein", "static"],
    },
    {
        "name": "frontier_defer4_p090",
        "p": "0.90",
        "args": [
            "--dynamic-safein", "frontier",
            "--dynamic-safein-stable-probes", "1",
            "--dynamic-safein-defer-initial-clusters", "4",
            "--dynamic-safein-defer-until-ready", "1",
        ],
    },
]


EXTRA_TOPK50 = [
    {
        "name": "frontier_defer4_p085",
        "p": "0.85",
        "args": [
            "--dynamic-safein", "frontier",
            "--dynamic-safein-stable-probes", "1",
            "--dynamic-safein-defer-initial-clusters", "4",
            "--dynamic-safein-defer-until-ready", "1",
        ],
    },
]


def sample_path(topk: int) -> Path:
    if topk == 10:
        prior = ROOT / "dynamic_prefetch_runs" / "safein_exact_dk_samples.npy"
        if prior.exists():
            return prior
    return RUN_ROOT / f"safein_exact_dk_samples_topk{topk}.npy"


def base_cmd(outdir: Path, per_query: Path, topk: int, percentile: str) -> list[str]:
    return [
        str(BENCH),
        "--base", str(BASE),
        "--query", str(QUERY),
        "--image-ids", str(IMAGE_IDS),
        "--index-dir", str(INDEX_DIR),
        "--outdir", str(outdir),
        "--per-query-stats-output", str(per_query),
        "--nlist", "2048",
        "--nprobe", "64",
        "--topk", str(topk),
        "--bits", "4",
        "--metric", "cosine",
        "--queries", "1000",
        "--dynamic-safeout", "1",
        "--safein-dk-space", "exact_l2",
        "--safein-dk-percentile", percentile,
        "--safein-dk-samples", "1000",
        "--epsilon-sampling-mode", "legacy_per_cluster",
    ]


def summarize(name: str, topk: int, outdir: Path) -> dict[str, object]:
    with (outdir / "results.json").open() as f:
        r = json.load(f)
    total_classified_safein = r.get("s1_safein", 0) + r.get("s2_safein", 0)
    total_classified_false = r.get("s1_false_safein", 0) + r.get("s2_false_safein", 0)
    coverage = r.get("safein_prefetch_topk_coverage", 0.0)
    false_rate = r.get("safein_prefetch_false_rate", 0.0)
    return {
        "name": name,
        "topk": topk,
        "target_hit": coverage >= 0.20 and false_rate < 0.20,
        "dynamic_safein_mode": r.get("dynamic_safein_mode"),
        "safein_d_k": r.get("safein_d_k"),
        "recall_at_k": r.get("recall_at_k"),
        "latency_avg_ms": r.get("latency_avg_ms"),
        "latency_p95_ms": r.get("latency_p95_ms"),
        "safein_prefetch_topk_coverage": coverage,
        "safein_prefetch_false_rate": false_rate,
        "safein_prefetch_candidates": r.get("safein_prefetch_candidates"),
        "safein_prefetch_true_topk": r.get("safein_prefetch_true_topk"),
        "safein_prefetch_false": r.get("safein_prefetch_false"),
        "dynamic_safein_deferred_candidates": r.get("dynamic_safein_deferred_candidates"),
        "dynamic_safein_deferred_safein": r.get("dynamic_safein_deferred_safein"),
        "all_read_requests": r.get("all_read_requests"),
        "remaining_payload_fetches": r.get("remaining_payload_fetches"),
        "classified_safein": total_classified_safein,
        "classified_false_safein": total_classified_false,
        "classified_false_rate": (
            total_classified_false / total_classified_safein
            if total_classified_safein else 0.0
        ),
    }


def run_one(scheme: dict[str, object], topk: int, manifest: list[dict[str, object]]) -> None:
    name = f"topk{topk}_{scheme['name']}"
    outdir = RUN_ROOT / name
    outdir.mkdir(parents=True, exist_ok=True)
    per_query = outdir / "online_per_query.csv"
    log_path = outdir / "run.log"
    samples = sample_path(topk)
    cmd = base_cmd(outdir, per_query, topk, str(scheme["p"]))
    if samples.exists():
        cmd += ["--safein-dk-samples-input", str(samples)]
    else:
        cmd += ["--safein-dk-samples-output", str(samples)]
    cmd += list(scheme["args"])
    (outdir / "cmd.txt").write_text(" ".join(cmd) + "\n")
    print(f"=== {name} ===", flush=True)
    with log_path.open("w") as log:
        proc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, text=True)
    if proc.returncode != 0:
        raise SystemExit(f"{name} failed; see {log_path}")
    manifest.append(summarize(name, topk, outdir))


def main() -> int:
    RUN_ROOT.mkdir(parents=True, exist_ok=True)
    if not BENCH.exists():
        raise SystemExit(f"missing benchmark binary: {BENCH}")

    manifest: list[dict[str, object]] = []
    for topk in (10, 50):
        schemes = list(SCHEMES)
        if topk == 50:
            schemes.extend(EXTRA_TOPK50)
        for scheme in schemes:
            run_one(scheme, topk, manifest)

    summary_json = RUN_ROOT / "summary.json"
    summary_csv = RUN_ROOT / "summary.csv"
    summary_json.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    with summary_csv.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(manifest[0].keys()))
        writer.writeheader()
        writer.writerows(manifest)
    print(f"Wrote {summary_json}")
    print(f"Wrote {summary_csv}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
