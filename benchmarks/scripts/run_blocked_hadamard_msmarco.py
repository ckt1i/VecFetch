#!/usr/bin/env python3
"""Run the MSMARCO rotation comparison for random/padded/blocked Hadamard.

This script prepares a bench_e2e-compatible MSMARCO adapter, builds or reuses
three indices at the same work point, and exports a compact report covering:
latency, recall@10, and index size breakdown.
"""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Sequence


@dataclass(frozen=True)
class RotationRun:
    name: str
    pad_to_pow2: bool
    blocked_hadamard_permuted: bool
    centroids: str


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Run MSMARCO random/padded/blocked Hadamard comparisons."
    )
    p.add_argument(
        "--bench-bin",
        default="/home/zcq/VDB/VectorRetrival/build/benchmarks/bench_e2e",
        help="Path to bench_e2e binary.",
    )
    p.add_argument(
        "--embeddings-root",
        default="/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings",
        help="MSMARCO embeddings root.",
    )
    p.add_argument(
        "--adapter-dir",
        default="/home/zcq/VDB/test/msmarco_blocked_hadamard_adapter",
        help="bench_e2e adapter output directory.",
    )
    p.add_argument(
        "--output-root",
        default="/home/zcq/VDB/test/msmarco_blocked_hadamard_compare",
        help="Directory for run outputs and summary reports.",
    )
    p.add_argument(
        "--gt-file",
        default="/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/gt/gt_top10.npy",
        help="Ground-truth top10 file.",
    )
    p.add_argument("--queries", type=int, default=1000)
    p.add_argument("--topk", type=int, default=10)
    p.add_argument("--nlist", type=int, default=16384)
    p.add_argument("--nprobe", type=int, default=256)
    p.add_argument("--bits", type=int, default=1)
    p.add_argument("--force-rebuild", action="store_true")
    return p.parse_args()


def run_cmd(cmd: Sequence[str], cwd: Path | None = None) -> None:
    subprocess.run(cmd, cwd=cwd, check=True)


def load_json(path: Path) -> Dict:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def prepare_adapter(args: argparse.Namespace) -> None:
    script = Path(__file__).with_name("prepare_msmarco_bench_e2e.py")
    cmd = [
        sys.executable,
        str(script),
        "--source-root",
        str(Path(args.embeddings_root).parent),
        "--output-dir",
        str(Path(args.adapter_dir)),
        "--gt-file",
        args.gt_file,
        "--prefer-symlink",
    ]
    run_cmd(cmd, cwd=script.parent)


def run_specs(args: argparse.Namespace) -> List[RotationRun]:
    root = Path(args.embeddings_root)
    return [
        RotationRun(
            name="random_matrix",
            pad_to_pow2=False,
            blocked_hadamard_permuted=False,
            centroids=str(root / "msmarco_passage_centroid_16384.fvecs"),
        ),
        RotationRun(
            name="padded_hadamard",
            pad_to_pow2=True,
            blocked_hadamard_permuted=False,
            centroids=str(root / "msmarco_passage_centroid_16384_pad1024.fvecs"),
        ),
        RotationRun(
            name="blocked_hadamard_permuted",
            pad_to_pow2=False,
            blocked_hadamard_permuted=True,
            centroids=str(root / "msmarco_passage_centroid_16384.fvecs"),
        ),
    ]


def bench_command(
    args: argparse.Namespace,
    spec: RotationRun,
    run_output: Path,
    index_dir: Path,
) -> List[str]:
    assignments = (
        Path(args.embeddings_root) / "msmarco_passage_cluster_id_16384.ivecs"
    )
    cmd = [
        str(Path(args.bench_bin)),
        "--dataset",
        str(Path(args.adapter_dir)),
        "--output",
        str(run_output),
        "--gt-file",
        args.gt_file,
        "--nlist",
        str(args.nlist),
        "--nprobe",
        str(args.nprobe),
        "--topk",
        str(args.topk),
        "--queries",
        str(args.queries),
        "--bits",
        str(args.bits),
        "--metric",
        "cosine",
        "--centroids",
        spec.centroids,
        "--assignments",
        str(assignments),
        "--pad-to-pow2",
        "1" if spec.pad_to_pow2 else "0",
        "--blocked-hadamard-permuted",
        "1" if spec.blocked_hadamard_permuted else "0",
        "--crc",
        "1",
        "--early-stop",
        "1",
        "--crc-alpha",
        "0.02",
        "--assignment-mode",
        "single",
        "--coarse-builder",
        "superkmeans",
    ]
    reuse_index = (not args.force_rebuild) and (index_dir / "segment.meta").exists()
    if reuse_index:
        cmd.extend(["--index-dir", str(index_dir), "--query-only", "1"])
    return cmd


def resolve_results_path(run_output: Path) -> Path:
    direct = run_output / "results.json"
    if direct.exists():
        return direct
    candidates = sorted(run_output.glob("*/results.json"))
    if candidates:
        return candidates[-1]
    raise FileNotFoundError(f"results.json not found under {run_output}")


def write_reports(output_root: Path, rows: List[Dict[str, object]]) -> None:
    csv_path = output_root / "rotation_compare.csv"
    json_path = output_root / "rotation_compare.json"
    md_path = output_root / "rotation_compare.md"

    fieldnames = [
        "name",
        "rotation_mode",
        "padding_mode",
        "logical_dimension",
        "effective_dimension",
        "nlist",
        "nprobe",
        "avg_query_time_ms",
        "recall_at_10",
        "index_cluster_clu_bytes",
        "index_rotated_centroids_bytes",
        "index_rotation_bytes",
        "index_total_bytes",
        "resolved_index_dir",
    ]
    with csv_path.open("w", newline="", encoding="utf-8") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writer.writerow(row)

    with json_path.open("w", encoding="utf-8") as f:
        json.dump(rows, f, indent=2, ensure_ascii=False)
        f.write("\n")

    with md_path.open("w", encoding="utf-8") as f:
        f.write("# MSMARCO blocked Hadamard 对照结果\n\n")
        f.write("| mode | rotation_mode | padding | logical_dim | effective_dim | nprobe | latency_ms | recall@10 | clu_bytes | rotated_centroids_bytes | total_bytes |\n")
        f.write("| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |\n")
        for row in rows:
            f.write(
                f"| {row['name']} | {row['rotation_mode']} | {row['padding_mode']} "
                f"| {row['logical_dimension']} | {row['effective_dimension']} "
                f"| {row['nprobe']} | {row['avg_query_time_ms']:.4f} "
                f"| {row['recall_at_10']:.4f} | {int(row['index_cluster_clu_bytes'])} "
                f"| {int(row['index_rotated_centroids_bytes'])} | {int(row['index_total_bytes'])} |\n"
            )
        f.write("\n")
        f.write("- 参考点：`random_matrix ~= 13ms`\n")
        f.write("- 参考点：`padded_hadamard ~= 7ms`, `recall@10 ~= 0.96`\n")


def main() -> int:
    args = parse_args()
    output_root = Path(args.output_root).resolve()
    output_root.mkdir(parents=True, exist_ok=True)
    prepare_adapter(args)

    rows: List[Dict[str, object]] = []
    for spec in run_specs(args):
        run_output = output_root / spec.name
        index_dir = output_root / "indices" / spec.name
        run_output.mkdir(parents=True, exist_ok=True)
        index_dir.parent.mkdir(parents=True, exist_ok=True)
        cmd = bench_command(args, spec, run_output, index_dir)
        run_cmd(cmd, cwd=run_output)

        data = load_json(resolve_results_path(run_output))
        metrics = data["metrics"]
        resolved_index_dir = Path(metrics.get("resolved_index_dir", str(index_dir)))
        if resolved_index_dir.exists() and resolved_index_dir != index_dir:
            index_dir.mkdir(parents=True, exist_ok=True)
        row = {
            "name": spec.name,
            "rotation_mode": metrics["rotation_mode"],
            "padding_mode": metrics["padding_mode"],
            "logical_dimension": metrics["logical_dimension"],
            "effective_dimension": metrics["effective_dimension"],
            "nlist": args.nlist,
            "nprobe": args.nprobe,
            "avg_query_time_ms": metrics["avg_query_time_ms"],
            "recall_at_10": metrics["recall_at_10"],
            "index_cluster_clu_bytes": metrics.get("index_cluster_clu_bytes", 0.0),
            "index_rotated_centroids_bytes": metrics.get("index_rotated_centroids_bytes", 0.0),
            "index_rotation_bytes": metrics.get("index_rotation_bytes", 0.0),
            "index_total_bytes": metrics.get("index_total_bytes", 0.0),
            "resolved_index_dir": str(resolved_index_dir),
        }
        rows.append(row)

    write_reports(output_root, rows)
    print(f"wrote reports under {output_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
