#!/usr/bin/env python3
"""Run COCO100k supported Dynamic SafeIn frontier experiments.

Outputs are written under safeIn_dk_review_logs/dynamic_prefetch_runs.
Historical upper/lower/scale/payload-only sweeps are no longer runnable after
the supported mode set was simplified to static/off/frontier.
"""

from __future__ import annotations

import csv
import json
import subprocess
from pathlib import Path


ROOT = Path(__file__).resolve().parent
REPO = ROOT.parent
RUN_ROOT = ROOT / "dynamic_prefetch_runs"
BENCH = REPO / "build" / "benchmarks" / "bench_vector_search"

DATA = Path("/home/zcq/VDB/data/coco_100k")
BASE = DATA / "image_embeddings.npy"
QUERY = DATA / "query_embeddings.npy"
IMAGE_IDS = DATA / "image_ids.npy"
INDEX_DIR = Path("/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90")
SAMPLES = RUN_ROOT / "safein_exact_dk_samples.npy"


SCHEMES = [
    {
        "name": "static_p090",
        "p": "0.90",
        "args": ["--dynamic-safein", "static"],
    },
    {
        "name": "static_p095",
        "p": "0.95",
        "args": ["--dynamic-safein", "static"],
    },
    {
        "name": "static_p097",
        "p": "0.97",
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


def base_cmd(outdir: Path, per_query: Path, percentile: str) -> list[str]:
    cmd = [
        str(BENCH),
        "--base", str(BASE),
        "--query", str(QUERY),
        "--image-ids", str(IMAGE_IDS),
        "--index-dir", str(INDEX_DIR),
        "--outdir", str(outdir),
        "--per-query-stats-output", str(per_query),
        "--nlist", "2048",
        "--nprobe", "64",
        "--topk", "10",
        "--bits", "4",
        "--metric", "cosine",
        "--queries", "1000",
        "--dynamic-safeout", "1",
        "--safein-dk-space", "exact_l2",
        "--safein-dk-percentile", percentile,
        "--safein-dk-samples", "1000",
        "--epsilon-sampling-mode", "legacy_per_cluster",
    ]
    if SAMPLES.exists():
        cmd += ["--safein-dk-samples-input", str(SAMPLES)]
    else:
        cmd += ["--safein-dk-samples-output", str(SAMPLES)]
    return cmd


def summarize_per_query(path: Path) -> dict[str, float]:
    if not path.exists():
        return {}
    total_safein = 0
    total_false = 0
    t_above_safein = 0
    t_above_false = 0
    dyn_disabled = 0
    dyn_active = 0
    with path.open(newline="") as f:
        for row in csv.DictReader(f):
            safein = int(row["total_safein"])
            false = int(row["total_false_safein"])
            total_safein += safein
            total_false += false
            if float(row["T_minus_Rq"]) > 0.0:
                t_above_safein += safein
                t_above_false += false
            dyn_disabled += int(row.get("dynamic_safein_disabled_clusters", 0))
            dyn_active += int(row.get("dynamic_safein_active_clusters", 0))
    return {
        "false_safein_rate": total_false / total_safein if total_safein else 0.0,
        "t_above_false_share": total_false and t_above_false / total_false or 0.0,
        "t_above_safein": t_above_safein,
        "t_above_false": t_above_false,
        "dynamic_active_clusters": dyn_active,
        "dynamic_disabled_clusters": dyn_disabled,
    }


def summarize_run(name: str, outdir: Path, per_query: Path) -> dict[str, object]:
    with (outdir / "results.json").open() as f:
        r = json.load(f)
    total_safein = r.get("s1_safein", 0) + r.get("s2_safein", 0)
    total_false = r.get("s1_false_safein", 0) + r.get("s2_false_safein", 0)
    row: dict[str, object] = {
        "name": name,
        "safein_d_k": r.get("safein_d_k"),
        "dynamic_safein_mode": r.get("dynamic_safein_mode"),
        "recall_at_10": r.get("recall_at_10"),
        "latency_avg_ms": r.get("latency_avg_ms"),
        "latency_p95_ms": r.get("latency_p95_ms"),
        "latency_p99_ms": r.get("latency_p99_ms"),
        "total_safein": total_safein,
        "false_safein": total_false,
        "false_safein_rate": total_false / total_safein if total_safein else 0.0,
        "s1_safein": r.get("s1_safein"),
        "s2_safein": r.get("s2_safein"),
        "vec_only_read_requests": r.get("vec_only_read_requests"),
        "all_read_requests": r.get("all_read_requests"),
        "payload_read_requests": r.get("payload_read_requests"),
        "safein_payload_prefetched": r.get("safein_payload_prefetched"),
        "remaining_payload_fetches": r.get("remaining_payload_fetches"),
        "dynamic_safein_active_clusters": r.get("dynamic_safein_active_clusters"),
        "dynamic_safein_disabled_clusters": r.get("dynamic_safein_disabled_clusters"),
        "dynamic_safein_threshold_avg": r.get("dynamic_safein_threshold_avg"),
    }
    row.update(summarize_per_query(per_query))
    return row


def main() -> int:
    RUN_ROOT.mkdir(parents=True, exist_ok=True)
    if not BENCH.exists():
        raise SystemExit(f"missing benchmark binary: {BENCH}")

    manifest: list[dict[str, object]] = []
    for scheme in SCHEMES:
        name = scheme["name"]
        outdir = RUN_ROOT / name
        outdir.mkdir(parents=True, exist_ok=True)
        per_query = outdir / "online_per_query.csv"
        log_path = outdir / "run.log"
        cmd = base_cmd(outdir, per_query, scheme["p"]) + scheme["args"]
        (outdir / "cmd.txt").write_text(" ".join(cmd) + "\n")
        print(f"=== {name} ===", flush=True)
        print(" ".join(cmd), flush=True)
        with log_path.open("w") as log:
            proc = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, text=True)
        if proc.returncode != 0:
            raise SystemExit(f"{name} failed; see {log_path}")
        manifest.append(summarize_run(name, outdir, per_query))

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
