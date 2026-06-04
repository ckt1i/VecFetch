#!/usr/bin/env python3
"""Run the fixed-parameter MSMARCO FHT-Kac mainline validation."""

from __future__ import annotations

import argparse
import csv
import json
import subprocess
import sys
from pathlib import Path
from typing import Dict, Sequence


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description="Run the MSMARCO FHT-Kac mainline validation."
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
        default="/home/zcq/VDB/test/msmarco_fht_kac_mainline",
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


def bench_command(args: argparse.Namespace, run_output: Path, index_dir: Path) -> list[str]:
    root = Path(args.embeddings_root)
    assignments = root / "msmarco_passage_cluster_id_16384.ivecs"
    centroids = root / "msmarco_passage_centroid_16384.fvecs"
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
        str(centroids),
        "--assignments",
        str(assignments),
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


def write_reports(output_root: Path, row: Dict[str, object]) -> None:
    csv_path = output_root / "fht_kac_mainline.csv"
    json_path = output_root / "fht_kac_mainline.json"
    md_path = output_root / "fht_kac_mainline.md"
    fieldnames = [
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
        writer.writerow(row)
    with json_path.open("w", encoding="utf-8") as f:
        json.dump(row, f, indent=2, ensure_ascii=False)
        f.write("\n")
    with md_path.open("w", encoding="utf-8") as f:
        f.write("# MSMARCO FHT-Kac mainline validation\n\n")
        f.write("| rotation | logical/effective | avg ms | recall@10 | total bytes |\n")
        f.write("| --- | --- | ---: | ---: | ---: |\n")
        f.write(
            f"| {row['rotation_mode']} | "
            f"{row['logical_dimension']}/{row['effective_dimension']} | "
            f"{row['avg_query_time_ms']:.4f} | {row['recall_at_10']:.4f} | "
            f"{int(row['index_total_bytes'])} |\n"
        )


def extract_row(args: argparse.Namespace, results: Dict) -> Dict[str, object]:
    metrics = results["metrics"]
    return {
        "rotation_mode": metrics["rotation_mode"],
        "padding_mode": metrics["padding_mode"],
        "logical_dimension": metrics["logical_dimension"],
        "effective_dimension": metrics["effective_dimension"],
        "nlist": args.nlist,
        "nprobe": args.nprobe,
        "avg_query_time_ms": metrics["avg_query_time_ms"],
        "recall_at_10": metrics["recall_at_10"],
        "index_cluster_clu_bytes": metrics.get("index_cluster_clu_bytes", 0.0),
        "index_rotated_centroids_bytes": metrics.get(
            "index_rotated_centroids_bytes", 0.0
        ),
        "index_rotation_bytes": metrics.get("index_rotation_bytes", 0.0),
        "index_total_bytes": metrics.get("index_total_bytes", 0.0),
        "resolved_index_dir": metrics.get("resolved_index_dir", ""),
    }


def main() -> int:
    args = parse_args()
    output_root = Path(args.output_root).resolve()
    run_output = output_root / "fht_kac_rotator"
    index_dir = output_root / "indices" / "fht_kac_rotator"
    output_root.mkdir(parents=True, exist_ok=True)
    run_output.mkdir(parents=True, exist_ok=True)
    index_dir.parent.mkdir(parents=True, exist_ok=True)

    prepare_adapter(args)
    run_cmd(bench_command(args, run_output, index_dir), cwd=run_output)
    row = extract_row(args, load_json(resolve_results_path(run_output)))
    write_reports(output_root, row)
    print(json.dumps(row, indent=2, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
