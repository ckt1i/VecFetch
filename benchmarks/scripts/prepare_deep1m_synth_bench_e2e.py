#!/usr/bin/env python3
"""Prepare a Deep1M_synth adapter directory for bench_e2e."""

from __future__ import annotations

import argparse
import json
import os
import shutil
from pathlib import Path

import numpy as np


def _ensure_parent(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)


def _replace_path(dst: Path) -> None:
    if dst.exists() or dst.is_symlink():
        if dst.is_dir() and not dst.is_symlink():
            raise RuntimeError(f"Refusing to replace directory: {dst}")
        dst.unlink()


def _link_or_copy(src: Path, dst: Path, prefer_symlink: bool) -> str:
    _ensure_parent(dst)
    _replace_path(dst)
    if prefer_symlink:
        try:
            os.symlink(src, dst)
            return "symlink"
        except OSError:
            pass
    shutil.copy2(src, dst)
    return "copy"


def main() -> int:
    parser = argparse.ArgumentParser(description="Prepare Deep1M_synth bench_e2e adapter.")
    parser.add_argument(
        "--source-root",
        default="/home/zcq/VDB/baselines/data/formal_baselines/deep1m_synth",
        help="Deep1M_synth formatted root directory.",
    )
    parser.add_argument(
        "--raw-root",
        default="/home/zcq/VDB/data/formal_baselines/deep1m_synth/embeddings",
        help="Deep1M_synth raw embeddings directory.",
    )
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--gt-file", default="")
    parser.add_argument("--prefer-symlink", action="store_true")
    parser.add_argument("--query-limit", type=int, default=0)
    args = parser.parse_args()

    source_root = Path(args.source_root)
    raw_root = Path(args.raw_root)
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    base_emb = raw_root / "base_embeddings.npy"
    query_emb = raw_root / "query_embeddings.npy"
    split = json.loads((source_root / "splits" / "split_v1.json").read_text())
    query_indices = np.asarray(split["query_indices"], dtype=np.int64)
    if args.query_limit > 0:
        query_indices = query_indices[: args.query_limit]

    base_arr = np.load(base_emb, mmap_mode="r")
    query_arr = np.load(query_emb, mmap_mode="r")
    if base_arr.ndim != 2 or query_arr.ndim != 2:
        raise RuntimeError("Expected 2-D embedding arrays")

    base_mode = _link_or_copy(base_emb, out_dir / "image_embeddings.npy", args.prefer_symlink)
    _ensure_parent(out_dir / "query_embeddings.npy")
    _replace_path(out_dir / "query_embeddings.npy")
    np.save(out_dir / "query_embeddings.npy", np.asarray(query_arr[query_indices], dtype=np.float32))

    image_ids = np.arange(base_arr.shape[0], dtype=np.int64)
    query_ids = query_indices.astype(np.int64, copy=False)
    np.save(out_dir / "image_ids.npy", image_ids)
    np.save(out_dir / "query_ids.npy", query_ids)

    metadata_path = out_dir / "metadata.jsonl"
    with metadata_path.open("w", encoding="utf-8") as dst:
        for row_id in range(base_arr.shape[0]):
            record = {
                "image_id": int(row_id),
                "caption": f"deep1m_synth_doc_{row_id}",
                "doc_id": int(row_id),
                "source_row_id": int(row_id),
            }
            dst.write(json.dumps(record, ensure_ascii=False) + "\n")

    gt_out = ""
    gt_mode = "none"
    if args.gt_file:
        gt_src = Path(args.gt_file)
        gt_out_path = out_dir / gt_src.name
        gt_mode = _link_or_copy(gt_src, gt_out_path, args.prefer_symlink)
        gt_out = str(gt_out_path)

    manifest = {
        "source_root": str(source_root),
        "raw_root": str(raw_root),
        "output_dir": str(out_dir),
        "base_embeddings": str(base_emb),
        "query_embeddings": str(query_emb),
        "base_rows": int(base_arr.shape[0]),
        "query_rows": int(query_ids.shape[0]),
        "dim": int(base_arr.shape[1]),
        "metric": "l2",
        "normalization": "l2_unit_norm",
        "metric_equivalence_note": "公开 Deep1M 向量已经做 L2 归一化，因此本实验中的欧氏距离（L2）排序与余弦相似度排序等价。",
        "files": {
            "image_embeddings.npy": base_mode,
            "query_embeddings.npy": f"slice_{query_ids.shape[0]}",
            "image_ids.npy": "generated",
            "query_ids.npy": "generated",
            "metadata.jsonl": "generated",
        },
        "gt_file": gt_out,
        "gt_file_mode": gt_mode,
        "id_mappings": {
            "image_ids": "row_id",
            "query_ids": "split_v1.query_indices",
            "metadata.image_id": "row_id",
        },
    }
    with (out_dir / "adapter_manifest.json").open("w", encoding="utf-8") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)
        handle.write("\n")
    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
