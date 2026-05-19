#!/usr/bin/env python3
"""Run repeatable hot-path experiments for bench_e2e.

This script executes a repeatable experiment matrix against the existing
benchmark binary, collects `results.json` from each run, and optionally wraps
each run in `perf stat` via the existing shell helper.

The current default matrix is the hot-path experiment plan v1:
- fixed-probe baseline (`nprobe=64`, CRC enabled, early-stop disabled)
- CRC early-stop baseline (`nprobe=256`, CRC enabled, `crc-alpha=0.02`)

The intent is to keep the benchmark binary unchanged while making it easy to
compare baseline and currently-supported control runs in a single pass.
"""

from __future__ import annotations

import argparse
import csv
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from statistics import mean, pstdev
from typing import Dict, List, Sequence


@dataclass(frozen=True)
class RunSpec:
    name: str
    extra_args: Sequence[str]
    profile: str
    stage2_breakdown_mode: str


def arg_value(extra_args: Sequence[str], flag: str, default: str) -> str:
    for i, token in enumerate(extra_args):
        if token == flag and i + 1 < len(extra_args):
            return str(extra_args[i + 1])
    return default


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Run repeatable hot-path experiments for bench_e2e."
    )
    p.add_argument("--bench-bin", required=True, help="Path to bench_e2e binary")
    p.add_argument("--dataset", required=True, help="Dataset path")
    p.add_argument("--output-root", required=True, help="Directory for experiment outputs")
    p.add_argument("--index-dir", required=True, help="Index directory")
    p.add_argument("--queries", type=int, default=1000)
    p.add_argument("--topk", type=int, default=10)
    p.add_argument("--nprobe", type=int, default=64)
    p.add_argument("--bits", type=int, default=4)
    p.add_argument("--repeats", type=int, default=3)
    p.add_argument("--run-perf", action="store_true", help="Wrap each run with perf stat")
    p.add_argument("--skip-crc", action="store_true", help="Run with --crc 0")
    p.add_argument(
        "--mode",
        choices=("legacy-baseline", "legacy-baseline-only", "plan-v1", "plan-v1-baselines"),
        default="plan-v1",
        help=(
            "Experiment matrix to run. "
            "plan-v1 is the two-baseline hot-path validation matrix."
        ),
    )
    return p.parse_args()


def build_matrix(args: argparse.Namespace) -> List[RunSpec]:
    common = [
        "--dataset",
        args.dataset,
        "--index-dir",
        args.index_dir,
        "--topk",
        str(args.topk),
        "--queries",
        str(args.queries),
        "--bits",
        str(args.bits),
        "--clu-read-mode",
        "full_preload",
        "--use-resident-clusters",
        "1",
    ]

    if args.mode.startswith("legacy-baseline"):
        legacy_common = common + [
            "--nprobe",
            str(args.nprobe),
            "--early-stop",
            "0",
            "--crc",
            "0" if args.skip_crc else "1",
        ]
        matrix = [
            RunSpec(
                "baseline",
                legacy_common,
                profile="legacy_fixed_probe",
                stage2_breakdown_mode="low_overhead_or_unmeasured",
            ),
        ]
        if args.mode == "legacy-baseline-only":
            return matrix

        matrix.extend(
            [
                RunSpec(
                    "no-resident",
                    legacy_common + ["--use-resident-clusters", "0"],
                    profile="legacy_fixed_probe",
                    stage2_breakdown_mode="low_overhead_or_unmeasured",
                ),
                RunSpec(
                    "window-read",
                    legacy_common + ["--clu-read-mode", "window"],
                    profile="legacy_fixed_probe",
                    stage2_breakdown_mode="low_overhead_or_unmeasured",
                ),
                RunSpec(
                    "submit-batch-0",
                    legacy_common + ["--submit-batch", "0"],
                    profile="legacy_fixed_probe",
                    stage2_breakdown_mode="low_overhead_or_unmeasured",
                ),
                RunSpec(
                    "submit-batch-32",
                    legacy_common + ["--submit-batch", "32"],
                    profile="legacy_fixed_probe",
                    stage2_breakdown_mode="low_overhead_or_unmeasured",
                ),
            ]
        )
        return matrix

    fixed_probe = common + [
        "--nprobe",
        "64",
        "--early-stop",
        "0",
        "--crc",
        "1",
    ]
    crc_early_stop = common + [
        "--nprobe",
        "256",
        "--early-stop",
        "1",
        "--crc",
        "1",
        "--crc-alpha",
        "0.02",
    ]

    matrix = [
        RunSpec(
            "fixed-probe-baseline",
            fixed_probe,
            profile="fixed_probe_baseline",
            stage2_breakdown_mode="low_overhead_or_unmeasured",
        ),
        RunSpec(
            "crc-early-stop-baseline",
            crc_early_stop,
            profile="crc_early_stop_baseline",
            stage2_breakdown_mode="low_overhead_or_unmeasured",
        ),
    ]

    if args.mode == "plan-v1-baselines":
        return matrix

    matrix.extend(
        [
            RunSpec(
                "fixed-probe-submit-batch-0",
                fixed_probe + ["--submit-batch", "0"],
                profile="fixed_probe_submit_controls",
                stage2_breakdown_mode="low_overhead_or_unmeasured",
            ),
            RunSpec(
                "fixed-probe-submit-batch-32",
                fixed_probe + ["--submit-batch", "32"],
                profile="fixed_probe_submit_controls",
                stage2_breakdown_mode="low_overhead_or_unmeasured",
            ),
        ]
    )
    return matrix


def run_cmd(cmd: Sequence[str], cwd: Path | None = None) -> None:
    subprocess.run(cmd, cwd=cwd, check=True)


def load_json(path: Path) -> Dict:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def summary_stats(values: Sequence[float]) -> Dict[str, float]:
    if not values:
        return {"mean": 0.0, "std": 0.0}
    if len(values) == 1:
        return {"mean": values[0], "std": 0.0}
    return {"mean": mean(values), "std": pstdev(values)}


def main() -> int:
    args = parse_args()
    bench_bin = Path(args.bench_bin).resolve()
    output_root = Path(args.output_root).resolve()
    output_root.mkdir(parents=True, exist_ok=True)

    if not bench_bin.exists():
        print(f"bench binary not found: {bench_bin}", file=sys.stderr)
        return 2

    matrix = build_matrix(args)
    results: List[Dict[str, object]] = []

    perf_helper = Path(__file__).with_name("run_perf_profile.sh")
    has_perf_helper = perf_helper.exists()

    for spec in matrix:
        run_root = output_root / spec.name
        run_root.mkdir(parents=True, exist_ok=True)
        run_jsons: List[Dict] = []
        run_times: List[float] = []
        run_recall: List[float] = []

        for repeat in range(args.repeats):
            trial_dir = run_root / f"repeat_{repeat:02d}"
            trial_dir.mkdir(parents=True, exist_ok=True)
            bench_out = trial_dir / "bench_output"
            bench_out.mkdir(parents=True, exist_ok=True)

            cmd = [str(bench_bin), "--output", str(bench_out), *spec.extra_args]
            if args.run_perf and has_perf_helper:
                perf_dir = trial_dir / "perf"
                env = os.environ.copy()
                env["BENCH_BIN"] = str(bench_bin)
                env["BENCH_ARGS"] = " ".join(cmd[1:])
                env["OUT_DIR"] = str(perf_dir)
                subprocess.run(
                    ["bash", str(perf_helper)],
                    check=True,
                    env=env,
                    cwd=perf_helper.parent,
                )
            else:
                subprocess.run(cmd, check=True, cwd=bench_out)

            # bench_e2e writes its results under the requested output directory.
            result_file = bench_out / "results.json"
            if not result_file.exists():
                # Some benchmark invocations may emit into the default output dir.
                candidates = sorted(bench_out.glob("*/results.json"))
                if candidates:
                    result_file = candidates[-1]
            if not result_file.exists():
                raise FileNotFoundError(f"results.json not found for {spec.name} repeat {repeat}")

            data = load_json(result_file)
            metrics = data["metrics"]
            pipeline = data["pipeline_stats"]
            row = {
                "name": spec.name,
                "profile": spec.profile,
                "stage2_breakdown_mode": spec.stage2_breakdown_mode,
                "repeat": repeat,
                "nprobe": int(arg_value(spec.extra_args, "--nprobe", str(args.nprobe))),
                "early_stop": int(arg_value(spec.extra_args, "--early-stop", "0")),
                "crc": int(arg_value(spec.extra_args, "--crc", "1")),
                "crc_alpha": float(arg_value(spec.extra_args, "--crc-alpha", "0.1")),
                "submit_batch": int(arg_value(spec.extra_args, "--submit-batch", "-1")),
                "submit_online": int(arg_value(spec.extra_args, "--submit-online", "0")),
                "stage2_block_first": int(arg_value(spec.extra_args, "--stage2-block-first", "1")),
                "stage2_batch_classify": int(arg_value(spec.extra_args, "--stage2-batch-classify", "1")),
                "avg_query_ms": metrics["avg_query_time_ms"],
                "recall_at_10": metrics.get("recall_at_10", 0.0),
                "coarse_score_ms": pipeline.get("avg_coarse_score_ms", 0.0),
                "coarse_topn_ms": pipeline.get("avg_coarse_topn_ms", 0.0),
                "probe_prepare_ms": pipeline.get("avg_probe_prepare_ms", 0.0),
                "probe_stage1_ms": pipeline.get("avg_probe_stage1_ms", 0.0),
                "probe_stage2_ms": pipeline.get("avg_probe_stage2_ms", 0.0),
                "probe_stage2_collect_ms": pipeline.get("avg_probe_stage2_collect_ms", 0.0),
                "probe_stage2_kernel_ms": pipeline.get("avg_probe_stage2_kernel_ms", 0.0),
                "probe_stage2_scatter_ms": pipeline.get("avg_probe_stage2_scatter_ms", 0.0),
                "probe_submit_ms": pipeline.get("avg_probe_submit_ms", 0.0),
                "probe_submit_prepare_vec_only_ms": pipeline.get(
                    "avg_probe_submit_prepare_vec_only_ms", 0.0
                ),
                "probe_submit_prepare_all_ms": pipeline.get(
                    "avg_probe_submit_prepare_all_ms", 0.0
                ),
                "probe_submit_emit_ms": pipeline.get("avg_probe_submit_emit_ms", 0.0),
                "submit_window_flushes": pipeline.get("avg_submit_window_flushes", 0.0),
                "submit_window_tail_flushes": pipeline.get(
                    "avg_submit_window_tail_flushes", 0.0
                ),
                "submit_stop_flushes": pipeline.get("avg_submit_stop_flushes", 0.0),
                "submit_window_requests": pipeline.get("avg_submit_window_requests", 0.0),
                "avg_probed_clusters": pipeline.get("avg_probed_clusters", 0.0),
                "avg_total_probed": pipeline.get("avg_total_probed", 0.0),
                "early_stopped_pct": pipeline.get("early_stopped_pct", 0.0),
                "avg_crc_would_stop": pipeline.get("avg_crc_would_stop", 0.0),
                "avg_crc_decision_ms": pipeline.get("avg_crc_decision_ms", 0.0),
                "avg_crc_buffer_ms": pipeline.get("avg_crc_buffer_ms", 0.0),
                "avg_crc_merge_ms": pipeline.get("avg_crc_merge_ms", 0.0),
                "avg_crc_online_ms": pipeline.get("avg_crc_online_ms", 0.0),
                "candidate_collect_ms": pipeline.get("avg_candidate_collect_ms", 0.0),
                "rerank_compute_ms": pipeline.get("avg_rerank_compute_ms", 0.0),
                "fetch_missing_ms": pipeline.get("avg_fetch_missing_ms", 0.0),
            }
            run_jsons.append(data)
            run_times.append(row["avg_query_ms"])
            run_recall.append(row["recall_at_10"])
            results.append(row)

        summary = {
            "name": spec.name,
            "profile": spec.profile,
            "stage2_breakdown_mode": spec.stage2_breakdown_mode,
            "repeats": args.repeats,
            "avg_query_ms": summary_stats(run_times),
            "recall_at_10": summary_stats(run_recall),
        }
        with (run_root / "summary.json").open("w", encoding="utf-8") as f:
            json.dump(summary, f, indent=2, sort_keys=True)

    csv_path = output_root / "hotpath_experiments.csv"
    fieldnames = [
        "name",
        "profile",
        "stage2_breakdown_mode",
        "repeat",
        "nprobe",
        "early_stop",
        "crc",
        "crc_alpha",
        "submit_batch",
        "submit_online",
        "stage2_block_first",
        "stage2_batch_classify",
        "avg_query_ms",
        "recall_at_10",
        "coarse_score_ms",
        "coarse_topn_ms",
        "probe_prepare_ms",
        "probe_stage1_ms",
        "probe_stage2_ms",
        "probe_stage2_collect_ms",
        "probe_stage2_kernel_ms",
        "probe_stage2_scatter_ms",
        "probe_submit_ms",
        "probe_submit_prepare_vec_only_ms",
        "probe_submit_prepare_all_ms",
        "probe_submit_emit_ms",
        "submit_window_flushes",
        "submit_window_tail_flushes",
        "submit_stop_flushes",
        "submit_window_requests",
        "avg_probed_clusters",
        "avg_total_probed",
        "early_stopped_pct",
        "avg_crc_would_stop",
        "avg_crc_decision_ms",
        "avg_crc_buffer_ms",
        "avg_crc_merge_ms",
        "avg_crc_online_ms",
        "candidate_collect_ms",
        "rerank_compute_ms",
        "fetch_missing_ms",
    ]
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for row in results:
            writer.writerow(row)

    print(f"wrote {csv_path}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
