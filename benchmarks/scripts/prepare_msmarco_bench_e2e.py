#!/usr/bin/env python3
"""Prepare a temporary MSMARCO adapter directory for bench_e2e.

This script maps MSMARCO formal-baseline assets into the COCO-style file
contract expected by bench_e2e:

  image_embeddings.npy  <- base_embeddings.npy
  query_embeddings.npy  <- query_embeddings.npy
  image_ids.npy         <- passage pid ids
  query_ids.npy         <- query qid ids
  metadata.jsonl        <- one JSON record per passage (pid/text/title)
  <gt file>             <- optional precomputed ground truth

The adapter uses the cleaned parquet assets so that ids match MSMARCO qrels
and precomputed ground truth. The script prefers symlinks for large arrays and
falls back to copies when symlinks are unavailable.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
from pathlib import Path

import numpy as np
import pyarrow.parquet as pq


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


def _load_shape(path: Path) -> tuple[int, int]:
    arr = np.load(path, mmap_mode="r")
    if arr.ndim != 2:
        raise RuntimeError(f"Expected 2-D embeddings array: {path}")
    return int(arr.shape[0]), int(arr.shape[1])


def _save_npy_slice(src: Path, dst: Path, limit: int | None) -> int:
    arr = np.load(src, mmap_mode="r")
    if arr.ndim != 2:
        raise RuntimeError(f"Expected 2-D embeddings array: {src}")
    rows = arr.shape[0]
    if limit is None or limit >= rows:
        return rows
    _ensure_parent(dst)
    _replace_path(dst)
    np.save(dst, np.asarray(arr[:limit]))
    return limit


def _write_ids(path: Path, ids: np.ndarray) -> None:
    if ids.ndim != 1:
        raise RuntimeError(f"Expected 1-D id array for {path}")
    _ensure_parent(path)
    np.save(path, ids)


def _load_parquet_columns(path: Path, columns: list[str]) -> dict[str, np.ndarray]:
    table = pq.read_table(path, columns=columns)
    data: dict[str, np.ndarray] = {}
    for name in columns:
        data[name] = table.column(name).to_numpy(zero_copy_only=False)
    return data


def _stream_ids_and_metadata(
    passages_path: Path,
    id_output_path: Path,
    metadata_output_path: Path,
    count: int,
) -> np.ndarray:
    parquet_file = pq.ParquetFile(passages_path)
    _ensure_parent(id_output_path)
    _ensure_parent(metadata_output_path)
    ids = np.empty(count, dtype=np.int64)
    written = 0
    with metadata_output_path.open("w", encoding="utf-8") as dst:
        for batch in parquet_file.iter_batches(
            batch_size=32768,
            columns=["row_id", "pid", "passage_text", "title"],
        ):
            row_id = batch.column("row_id").to_numpy(zero_copy_only=False).astype(
                np.int64, copy=False
            )
            pid = batch.column("pid").to_numpy(zero_copy_only=False).astype(
                np.int64, copy=False
            )
            passage_text = batch.column("passage_text").to_pylist()
            title = batch.column("title").to_pylist()
            batch_len = int(batch.num_rows)
            remaining = count - written
            if remaining <= 0:
                break
            take = min(batch_len, remaining)
            ids[written:written + take] = pid[:take]
            for i in range(take):
                record = {
                    "image_id": int(pid[i]),
                    "caption": passage_text[i],
                    "title": title[i],
                    "source_row_id": int(row_id[i]),
                    "source_pid": int(pid[i]),
                }
                dst.write(json.dumps(record, ensure_ascii=False) + "\n")
            written += take
            if written >= count:
                break
    if written < count:
        raise RuntimeError(
            f"passages.parquet has fewer rows ({written}) than requested ({count})"
        )
    np.save(id_output_path, ids)
    return ids


def _stream_query_ids(queries_path: Path, query_limit: int | None) -> np.ndarray:
    parquet_file = pq.ParquetFile(queries_path)
    qids: list[int] = []
    for batch in parquet_file.iter_batches(batch_size=32768, columns=["qid"]):
        batch_qids = batch.column("qid").to_numpy(zero_copy_only=False).astype(
            np.int64, copy=False
        )
        if query_limit is not None:
            remaining = query_limit - len(qids)
            if remaining <= 0:
                break
            batch_qids = batch_qids[:remaining]
        qids.extend(int(x) for x in batch_qids)
        if query_limit is not None and len(qids) >= query_limit:
            break
    if query_limit is not None and len(qids) < query_limit:
        raise RuntimeError(
            f"queries.parquet has fewer rows ({len(qids)}) than requested ({query_limit})"
        )
    return np.asarray(qids, dtype=np.int64)


def _write_metadata(
    passages_path: Path, output_path: Path, count: int, limit_rows: int | None
) -> np.ndarray:
    _ensure_parent(output_path)
    columns = ["row_id", "pid", "passage_text", "title"]
    table = pq.read_table(passages_path, columns=columns)
    if table.num_rows < count:
        raise RuntimeError(
            f"passages.parquet has fewer rows ({table.num_rows}) than embeddings ({count})"
        )
    if limit_rows is not None:
        table = table.slice(0, count)
    pid = table.column("pid").to_numpy(zero_copy_only=False).astype(np.int64, copy=False)
    row_id = table.column("row_id").to_numpy(zero_copy_only=False).astype(np.int64, copy=False)
    passage_text = table.column("passage_text").to_pylist()
    title = table.column("title").to_pylist()
    with output_path.open("w", encoding="utf-8") as dst:
        for i in range(count):
            record = {
                "image_id": int(pid[i]),
                "caption": passage_text[i],
                "title": title[i],
                "source_row_id": int(row_id[i]),
                "source_pid": int(pid[i]),
            }
            dst.write(json.dumps(record, ensure_ascii=False) + "\n")
    return pid


def _load_gt_pid_subset(
    gt_file: Path, query_limit: int, gt_k: int = 10
) -> np.ndarray:
    arr = np.load(gt_file, mmap_mode="r")
    if arr.ndim != 2:
        raise RuntimeError(f"Expected 2-D GT array: {gt_file}")
    rows = min(query_limit, arr.shape[0])
    if arr.shape[1] < gt_k:
        raise RuntimeError(f"GT file has fewer than {gt_k} columns: {gt_file}")
    return np.unique(np.asarray(arr[:rows, :gt_k], dtype=np.int64).reshape(-1))


def _subset_table_by_pid(
    passages_path: Path, allowed_pids: np.ndarray
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray]:
    table = pq.read_table(passages_path, columns=["row_id", "pid", "passage_text", "title"])
    row_id = table.column("row_id").to_numpy(zero_copy_only=False).astype(np.int64, copy=False)
    pid = table.column("pid").to_numpy(zero_copy_only=False).astype(np.int64, copy=False)
    text = np.asarray(table.column("passage_text").to_pylist(), dtype=object)
    title = np.asarray(table.column("title").to_pylist(), dtype=object)
    if allowed_pids.size == 0:
        raise RuntimeError("Allowed passage pid subset is empty")
    mask = np.isin(pid, allowed_pids)
    if not np.any(mask):
        raise RuntimeError("No passage rows matched the requested GT pid subset")
    order = np.argsort(row_id[mask], kind="stable")
    return row_id[mask][order], pid[mask][order], text[mask][order], title[mask][order]


def _subset_query_ids(queries_path: Path, query_limit: int | None) -> np.ndarray:
    table = pq.read_table(queries_path, columns=["qid"])
    qids = table.column("qid").to_numpy(zero_copy_only=False).astype(np.int64, copy=False)
    if query_limit is not None:
        qids = qids[:query_limit]
    return qids


def _first_existing_path(candidates: list[Path]) -> Path:
    for path in candidates:
        if path.is_file():
            return path
    raise FileNotFoundError(
        "None of the candidate files exist: " + ", ".join(str(p) for p in candidates)
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Prepare a temporary MSMARCO adapter directory for bench_e2e."
    )
    parser.add_argument(
        "--source-root",
        default="/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage",
        help="MSMARCO formal-baseline root directory.",
    )
    parser.add_argument(
        "--output-dir",
        required=True,
        help="Temporary bench_e2e adapter output directory.",
    )
    parser.add_argument(
        "--gt-file",
        default="",
        help="Optional precomputed ground-truth file to copy/link into the adapter directory.",
    )
    parser.add_argument(
        "--prefer-symlink",
        action="store_true",
        help="Prefer symlinks for large embedding files.",
    )
    parser.add_argument(
        "--limit-rows",
        type=int,
        default=0,
        help="Optional row limit for a smaller smoke adapter.",
    )
    parser.add_argument(
        "--query-limit",
        type=int,
        default=0,
        help="Optional query limit for a small smoke adapter.",
    )
    parser.add_argument(
        "--passage-limit-from-gt",
        action="store_true",
        help="When set with --gt-file and --query-limit, keep only passages appearing in the first query-limit GT rows.",
    )
    args = parser.parse_args()

    source_root = Path(args.source_root)
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    embeddings_root = _first_existing_path(
        [
            source_root / "embeddings" / "base_embeddings.npy",
            Path("/home/zcq/VDB/data/formal_baselines/msmarco_passage")
            / "embeddings"
            / "base_embeddings.npy",
            Path("/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage")
            / "embeddings"
            / "base_embeddings.npy",
        ]
    ).parent
    cleaned_root = _first_existing_path(
        [
            source_root / "cleaned" / "passages.parquet",
            Path("/home/zcq/VDB/baselines/data/formal_baselines/msmarco_passage")
            / "cleaned"
            / "passages.parquet",
            Path("/home/zcq/VDB/data/formal_baselines/msmarco_passage")
            / "cleaned"
            / "passages.parquet",
        ]
    ).parent
    base_emb = embeddings_root / "base_embeddings.npy"
    query_emb = embeddings_root / "query_embeddings.npy"
    passages_parquet = cleaned_root / "passages.parquet"
    queries_parquet = cleaned_root / "queries.parquet"
    embedding_meta = embeddings_root / "embedding_meta.json"

    base_rows, base_dim = _load_shape(base_emb)
    query_rows, query_dim = _load_shape(query_emb)
    if base_dim != query_dim:
        raise RuntimeError(
            f"Base/query embedding dims differ: {base_dim} vs {query_dim}"
        )

    limit_rows = args.limit_rows if args.limit_rows and args.limit_rows > 0 else None
    query_limit = args.query_limit if args.query_limit and args.query_limit > 0 else None
    if query_limit is not None:
        query_rows = min(query_rows, query_limit)
    if limit_rows is not None and not args.passage_limit_from_gt:
        base_rows = min(base_rows, limit_rows)

    if limit_rows is None and query_limit is None:
        link_mode_base = _link_or_copy(
            base_emb, out_dir / "image_embeddings.npy", args.prefer_symlink
        )
        link_mode_query = _link_or_copy(
            query_emb, out_dir / "query_embeddings.npy", args.prefer_symlink
        )
        passage_ids = _stream_ids_and_metadata(
            passages_parquet, out_dir / "image_ids.npy", out_dir / "metadata.jsonl", base_rows
        )
        query_ids = _stream_query_ids(queries_parquet, query_rows)
        _write_ids(out_dir / "query_ids.npy", query_ids)
    elif args.passage_limit_from_gt and args.gt_file:
        gt_pids = _load_gt_pid_subset(Path(args.gt_file), query_rows)
        passage_row_ids, passage_ids, passage_text, passage_title = _subset_table_by_pid(
            passages_parquet, gt_pids
        )
        query_ids = _subset_query_ids(queries_parquet, query_rows)
        base_arr = np.load(base_emb, mmap_mode="r")
        query_arr = np.load(query_emb, mmap_mode="r")
        if base_arr.ndim != 2 or query_arr.ndim != 2:
            raise RuntimeError("Expected 2-D embedding arrays")
        _ensure_parent(out_dir / "image_embeddings.npy")
        _replace_path(out_dir / "image_embeddings.npy")
        np.save(out_dir / "image_embeddings.npy", np.asarray(base_arr[passage_row_ids]))
        _ensure_parent(out_dir / "query_embeddings.npy")
        _replace_path(out_dir / "query_embeddings.npy")
        np.save(out_dir / "query_embeddings.npy", np.asarray(query_arr[:query_rows]))
        link_mode_base = f"gt_subset_{passage_ids.size}"
        link_mode_query = f"slice_{query_rows}"
        _write_ids(out_dir / "image_ids.npy", passage_ids)
        _write_ids(out_dir / "query_ids.npy", query_ids)
        with (out_dir / "metadata.jsonl").open("w", encoding="utf-8") as dst:
            for i in range(passage_ids.size):
                record = {
                    "image_id": int(passage_ids[i]),
                    "caption": passage_text[i],
                    "title": passage_title[i],
                    "source_row_id": int(passage_row_ids[i]),
                    "source_pid": int(passage_ids[i]),
                }
                dst.write(json.dumps(record, ensure_ascii=False) + "\n")
        base_rows = int(passage_ids.size)
    else:
        _save_npy_slice(base_emb, out_dir / "image_embeddings.npy", base_rows)
        _save_npy_slice(query_emb, out_dir / "query_embeddings.npy", query_rows)
        link_mode_base = f"slice_{base_rows}"
        link_mode_query = f"slice_{query_rows}"
        passage_table = pq.read_table(passages_parquet, columns=["row_id", "pid"])
        query_table = pq.read_table(queries_parquet, columns=["qid"])
        if passage_table.num_rows < base_rows:
            raise RuntimeError(
                f"passages.parquet has fewer rows ({passage_table.num_rows}) than embeddings ({base_rows})"
            )
        if query_table.num_rows < query_rows:
            raise RuntimeError(
                f"queries.parquet has fewer rows ({query_table.num_rows}) than embeddings ({query_rows})"
            )
        if limit_rows is not None:
            passage_table = passage_table.slice(0, base_rows)
            query_table = query_table.slice(0, query_rows)
        passage_ids = passage_table.column("pid").to_numpy(zero_copy_only=False).astype(np.int64, copy=False)
        query_ids = query_table.column("qid").to_numpy(zero_copy_only=False).astype(np.int64, copy=False)
        _write_ids(out_dir / "image_ids.npy", passage_ids)
        _write_ids(out_dir / "query_ids.npy", query_ids)
        metadata_ids = _write_metadata(passages_parquet, out_dir / "metadata.jsonl", base_rows, limit_rows)
        if not np.array_equal(metadata_ids, passage_ids):
            raise RuntimeError("metadata ids do not match passage ids")

    gt_out = ""
    gt_mode = "none"
    if args.gt_file:
        gt_src = Path(args.gt_file)
        if not gt_src.is_file():
            raise FileNotFoundError(gt_src)
        gt_out_path = out_dir / gt_src.name
        gt_mode = _link_or_copy(gt_src, gt_out_path, args.prefer_symlink)
        gt_out = str(gt_out_path)

    manifest = {
        "source_root": str(source_root),
        "embeddings_root": str(embeddings_root),
        "cleaned_root": str(cleaned_root),
        "output_dir": str(out_dir),
        "base_embeddings": str(base_emb),
        "query_embeddings": str(query_emb),
        "base_rows": base_rows,
        "query_rows": query_rows,
        "dim": base_dim,
        "limit_rows": limit_rows,
        "passages_parquet": str(passages_parquet),
        "queries_parquet": str(queries_parquet),
        "raw_compatibility": "cleaned parquet pid/qid",
        "files": {
            "image_embeddings.npy": link_mode_base,
            "query_embeddings.npy": link_mode_query,
            "image_ids.npy": "generated",
            "query_ids.npy": "generated",
            "metadata.jsonl": "generated",
        },
        "gt_file": gt_out,
        "gt_file_mode": gt_mode,
    }
    if embedding_meta.is_file():
        manifest["embedding_meta"] = str(embedding_meta)
    manifest["id_mappings"] = {
        "image_ids": "passages.pid",
        "query_ids": "queries.qid",
        "metadata.image_id": "passages.pid",
    }

    with (out_dir / "adapter_manifest.json").open("w", encoding="utf-8") as f:
        json.dump(manifest, f, ensure_ascii=False, indent=2)
        f.write("\n")

    print(json.dumps(manifest, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
