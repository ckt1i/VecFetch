#!/usr/bin/env python3
"""Run the fixed-parameter MSMARCO rotation comparison for padded/blocked/FhtKac."""

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
    fht_kac_rotator: bool
    centroids: str


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Run the fixed-parameter MSMARCO padded/blocked/FhtKac comparison."
    )
    p.add_argument(
        "--bench-bin",
        default="/home/zcq/VDB/VectorRetrival/build/benchmarks/bench_e2e",
    )
    p.add_argument(
        "--embeddings-root",
        default="/home/zcq/VDB/data/formal_baselines/msmarco_passage/embeddings",
    )
    p.add_argument(
        "--adapter-dir",
        default="/home/zcq/VDB/test/msmarco_fht_kac_adapter",
    )
    p.add_argument(
        "--output-root",
        default="/home/zcq/VDB/test/msmarco_fht_kac_compare",
    )
    p.add_argument(
        "--gt-file",
        default="/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage/gt/gt_top10.npy",
    )
    p.add_argument("--queries", type=int, default=1000)
    p.add_argument("--topk", type=int, default=10)
    p.add_argument("--nlist", type=int, default=16384)
    p.add_argument("--nprobe", type=int, default=256)
    p.add_argument("--bits", type=int, default=4)
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
            name="hadamard_padded",
            pad_to_pow2=True,
            blocked_hadamard_permuted=False,
            fht_kac_rotator=False,
            centroids=str(root / "msmarco_passage_centroid_16384_pad1024.fvecs"),
        ),
        RotationRun(
            name="blocked_hadamard_permuted",
            pad_to_pow2=False,
            blocked_hadamard_permuted=True,
            fht_kac_rotator=False,
            centroids=str(root / "msmarco_passage_centroid_16384.fvecs"),
        ),
        RotationRun(
            name="fht_kac_rotator",
            pad_to_pow2=False,
            blocked_hadamard_permuted=False,
            fht_kac_rotator=True,
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
        "--fht-kac-rotator",
        "1" if spec.fht_kac_rotator else "0",
        "--assignment-mode",
        "single",
        "--coarse-builder",
        "superkmeans",
        "--clu-read-mode",
        "full_preload",
        "--use-resident-clusters",
        "1",
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
        writer.writerows(rows)

    with json_path.open("w", encoding="utf-8") as f:
        json.dump(rows, f, indent=2)

    with md_path.open("w", encoding="utf-8") as f:
        f.write("| mode | logical/effective | avg ms | recall@10 | clu bytes | rotated centroids | rotation.bin | total bytes |\n")
        f.write("| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |\n")
        for row in rows:
            f.write(
                f"| {row['name']} | {row['logical_dimension']}/{row['effective_dimension']} | "
                f"{row['avg_query_time_ms']:.4f} | {row['recall_at_10']:.4f} | "
                f"{int(row['index_cluster_clu_bytes'])} | {int(row['index_rotated_centroids_bytes'])} | "
                f"{int(row['index_rotation_bytes'])} | {int(row['index_total_bytes'])} |\n"
            )


def extract_row(spec: RotationRun, results: Dict) -> Dict[str, object]:
    metrics = results["metrics"]
    index_meta = results["index"]
    return {
        "name": spec.name,
        "rotation_mode": index_meta["rotation_mode"],
        "padding_mode": index_meta["padding_mode"],
        "logical_dimension": index_meta["logical_dimension"],
        "effective_dimension": index_meta["effective_dimension"],
        "nlist": results["build"]["nlist"],
        "nprobe": results["query"]["nprobe"],
        "avg_query_time_ms": metrics["avg_query_time_ms"],
        "recall_at_10": metrics["recall_at_10"],
        "index_cluster_clu_bytes": metrics["index_cluster_clu_bytes"],
        "index_rotated_centroids_bytes": metrics["index_rotated_centroids_bytes"],
        "index_rotation_bytes": metrics["index_rotation_bytes"],
        "index_total_bytes": metrics["index_total_bytes"],
        "resolved_index_dir": results["build"]["resolved_index_dir"],
    }


def main() -> int:
    args = parse_args()
    output_root = Path(args.output_root)
    output_root.mkdir(parents=True, exist_ok=True)

    prepare_adapter(args)

    rows: List[Dict[str, object]] = []
    for spec in run_specs(args):
        run_output = output_root / spec.name
        index_dir = run_output / "index"
        run_output.mkdir(parents=True, exist_ok=True)
        run_cmd(bench_command(args, spec, run_output, index_dir))
        results = load_json(resolve_results_path(run_output))
        rows.append(extract_row(spec, results))

    write_reports(output_root, rows)
    print(json.dumps(rows, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
