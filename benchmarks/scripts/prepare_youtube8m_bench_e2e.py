#!/usr/bin/env python3
"""Prepare a YouTube-8M 1M formal-baseline dataset and bench_e2e adapter.

The script uses the official YouTube-8M frame-level TFRecord features. It does
not download raw videos. It aggregates per-second RGB/audio features into a
single L2-normalized video embedding:

  embedding = normalize(concat(mean(rgb_frames), mean(audio_frames)))

Outputs follow the formal-baseline layout:

  /home/zcq/VDB/data/formal_baselines/youtube8m_1m
  /home/zcq/VDB/baselines/data/formal_baselines/youtube8m_1m

Typical smoke run:

  python benchmarks/scripts/prepare_youtube8m_bench_e2e.py check-deps
  python benchmarks/scripts/prepare_youtube8m_bench_e2e.py all \\
    --target-videos 100 --target-queries 20 --skip-download \\
    --train-shards /path/to/train*.tfrecord \\
    --validate-shards /path/to/validate*.tfrecord

Full run after downloading shards:

  python benchmarks/scripts/prepare_youtube8m_bench_e2e.py all \\
    --target-videos 1000000 --target-queries 10000
"""

from __future__ import annotations

import argparse
import glob
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable, Sequence

import numpy as np
import pyarrow as pa
import pyarrow.parquet as pq
from numpy.lib.format import open_memmap

RAW_DATA_ROOT = Path("/home/zcq/VDB/data/formal_baselines")
FORMATTED_ROOT = Path("/home/zcq/VDB/baselines/data/formal_baselines")
DATASET = "youtube8m_1m"
DEFAULT_SEED = 20260531
DEFAULT_MIRRORS = ("asia", "us")
YT8M_DOWNLOAD_SCRIPT = "https://data.yt8m.org/download.py"
RGB_DIM = 1024
AUDIO_DIM = 128
EMBED_DIM = RGB_DIM + AUDIO_DIM
TOPK = 20


@dataclass
class VideoRecord:
    embedding: np.ndarray
    video_id: str
    labels: list[int]
    frame_count: int
    rgb_present: bool
    audio_present: bool
    source_shard: str


def now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S%z")


def raw_root() -> Path:
    return RAW_DATA_ROOT / DATASET


def raw_dir() -> Path:
    return raw_root() / "raw"


def raw_shards_dir() -> Path:
    return raw_dir() / "shards"


def embeddings_dir() -> Path:
    return raw_root() / "embeddings"


def formatted_root() -> Path:
    return FORMATTED_ROOT / DATASET


def cleaned_dir() -> Path:
    return formatted_root() / "cleaned"


def splits_dir() -> Path:
    return formatted_root() / "splits"


def gt_dir() -> Path:
    return formatted_root() / "gt"


def adapter_default_dir() -> Path:
    return raw_root() / "bench_e2e_adapter"


def ensure_dirs() -> None:
    for path in [
        raw_dir(),
        raw_shards_dir() / "train",
        raw_shards_dir() / "validate",
        embeddings_dir(),
        cleaned_dir(),
        splits_dir(),
        gt_dir(),
    ]:
        path.mkdir(parents=True, exist_ok=True)


def write_json(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")
    tmp.replace(path)


def read_json(path: Path, default: dict[str, Any] | None = None) -> dict[str, Any]:
    if not path.exists():
        return {} if default is None else dict(default)
    return json.loads(path.read_text(encoding="utf-8"))


def append_jsonl(path: Path, payload: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("a", encoding="utf-8") as handle:
        handle.write(json.dumps(payload, ensure_ascii=False) + "\n")


def free_gb(path: Path = RAW_DATA_ROOT) -> float:
    usage = shutil.disk_usage(path)
    return usage.free / (1024**3)


def import_tensorflow():
    try:
        import tensorflow as tf  # type: ignore
    except Exception as exc:  # pragma: no cover - depends on local env
        raise RuntimeError(
            "TensorFlow is required to parse YouTube-8M TFRecord files. "
            "Install tensorflow or tensorflow-cpu in the active environment."
        ) from exc
    return tf


def dependency_versions() -> dict[str, str]:
    versions = {
        "python": sys.version.split()[0],
        "platform": platform.platform(),
        "numpy": np.__version__,
        "pyarrow": pa.__version__,
    }
    try:
        import tensorflow as tf  # type: ignore

        versions["tensorflow"] = tf.__version__
    except Exception as exc:
        versions["tensorflow"] = f"missing: {exc.__class__.__name__}"
    return versions


def update_download_manifest(extra: dict[str, Any]) -> None:
    path = raw_dir() / "download_manifest.json"
    manifest = read_json(path)
    manifest.update(extra)
    manifest["dataset"] = DATASET
    manifest["updated_at"] = now()
    manifest["free_gb"] = round(free_gb(), 2)
    write_json(path, manifest)


def update_embedding_manifest(extra: dict[str, Any]) -> None:
    path = embeddings_dir() / "embedding_manifest.json"
    manifest = read_json(path)
    manifest.update(extra)
    manifest["dataset"] = DATASET
    manifest["updated_at"] = now()
    manifest["versions"] = dependency_versions()
    write_json(path, manifest)


def expand_patterns(patterns: Sequence[str], fallback_dir: Path) -> list[Path]:
    paths: list[Path] = []
    for pattern in patterns:
        matches = glob.glob(pattern)
        if matches:
            paths.extend(Path(match) for match in matches)
        else:
            paths.append(Path(pattern))
    if not paths:
        paths = sorted(fallback_dir.glob("*.tfrecord*"))
    paths = [path for path in paths if path.exists() and path.is_file()]
    return sorted(dict.fromkeys(paths))


def run_official_download(
    partition: str,
    shard_specs: Sequence[str],
    mirrors: Sequence[str],
    output_dir: Path,
) -> list[dict[str, Any]]:
    output_dir.mkdir(parents=True, exist_ok=True)
    records: list[dict[str, Any]] = []
    for shard_spec in shard_specs:
        success = False
        last_error = ""
        for mirror in mirrors:
            env = os.environ.copy()
            env.update({"partition": partition, "mirror": mirror, "shard": shard_spec})
            cmd = (
                "set -o pipefail; "
                f"curl -f -k -L {YT8M_DOWNLOAD_SCRIPT} | "
                f"{sys.executable}"
            )
            started = now()
            proc = subprocess.run(
                cmd,
                cwd=output_dir,
                env=env,
                executable="/bin/bash",
                shell=True,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
            )
            record = {
                "partition": partition,
                "shard": shard_spec,
                "mirror": mirror,
                "started_at": started,
                "finished_at": now(),
                "returncode": proc.returncode,
                "log_tail": proc.stdout[-4000:],
            }
            records.append(record)
            downloaded_files = sorted(
                str(path)
                for path in output_dir.glob("*")
                if path.is_file() and not path.name.endswith("_download_plan.json")
            )
            record["downloaded_file_count_after"] = len(downloaded_files)
            record["downloaded_files_tail"] = downloaded_files[-10:]
            failed_markers = (
                "curl:" in proc.stdout
                or "Traceback" in proc.stdout
                or "Error downloading" in proc.stdout
                or "MD5 does not match" in proc.stdout
            )
            if proc.returncode == 0 and not failed_markers:
                success = True
                break
            last_error = proc.stdout[-1000:]
        if not success:
            raise RuntimeError(
                f"Failed to download partition={partition} shard={shard_spec}: {last_error}"
            )
    return records


def shard_specs(count: int, denominator: int) -> list[str]:
    return [f"{idx},{denominator}" for idx in range(1, count + 1)]


def maybe_download(args: argparse.Namespace) -> None:
    ensure_dirs()
    mirrors = tuple(args.mirrors.split(",")) if args.mirrors else DEFAULT_MIRRORS
    download_records: list[dict[str, Any]] = []
    if args.skip_download:
        update_download_manifest({"download_skipped": True})
        return

    train_specs = args.train_shard_specs or shard_specs(
        args.train_shard_count, args.train_shard_denominator
    )
    validate_specs = args.validate_shard_specs or shard_specs(
        args.validate_shard_count, args.validate_shard_denominator
    )
    download_records.extend(
        run_official_download(
            "2/frame/train", train_specs, mirrors, raw_shards_dir() / "train"
        )
    )
    if args.target_queries > 0:
        download_records.extend(
            run_official_download(
                "2/frame/validate",
                validate_specs,
                mirrors,
                raw_shards_dir() / "validate",
            )
        )
    update_download_manifest(
        {
            "download_skipped": False,
            "download_records": download_records,
            "train_shard_specs": train_specs,
            "validate_shard_specs": validate_specs,
            "mirrors": list(mirrors),
            "official_download_script": YT8M_DOWNLOAD_SCRIPT,
        }
    )


def _decode_feature_list(
    feature_list: Any,
    dim: int,
) -> tuple[np.ndarray, bool]:
    frames: list[np.ndarray] = []
    for feature in feature_list.feature:
        values = feature.bytes_list.value
        if not values:
            continue
        arr = np.frombuffer(values[0], dtype=np.uint8)
        if arr.size != dim:
            continue
        frames.append(arr.astype(np.float32) / 255.0)
    if not frames:
        return np.zeros((dim,), dtype=np.float32), False
    stacked = np.stack(frames, axis=0)
    return stacked.mean(axis=0).astype(np.float32, copy=False), True


def _normalize(vec: np.ndarray) -> np.ndarray:
    norm = float(np.linalg.norm(vec))
    if norm > 0 and np.isfinite(norm):
        return (vec / norm).astype(np.float32, copy=False)
    return vec.astype(np.float32, copy=False)


def iter_records(shards: Sequence[Path]) -> Iterable[VideoRecord]:
    tf = import_tensorflow()
    for shard in shards:
        for raw in tf.data.TFRecordDataset(str(shard)):
            example = tf.train.SequenceExample()
            example.ParseFromString(bytes(raw.numpy()))
            context = example.context.feature
            video_bytes = context["id"].bytes_list.value
            video_id = video_bytes[0].decode("utf-8", errors="replace") if video_bytes else ""
            labels = [int(x) for x in context["labels"].int64_list.value]
            feature_lists = example.feature_lists.feature_list
            rgb, rgb_present = _decode_feature_list(feature_lists["rgb"], RGB_DIM)
            audio, audio_present = _decode_feature_list(feature_lists["audio"], AUDIO_DIM)
            frame_count = max(
                len(feature_lists["rgb"].feature),
                len(feature_lists["audio"].feature),
            )
            embedding = _normalize(np.concatenate([rgb, audio], axis=0))
            yield VideoRecord(
                embedding=embedding,
                video_id=video_id,
                labels=labels,
                frame_count=int(frame_count),
                rgb_present=rgb_present,
                audio_present=audio_present,
                source_shard=str(shard),
            )


def synthetic_blob(row_id: int, size: int = 256) -> str:
    seed = f"{DATASET}:{row_id}".encode("utf-8")
    out = bytearray()
    counter = 0
    while len(out) < size:
        out.extend(hashlib.sha256(seed + counter.to_bytes(4, "little")).hexdigest().encode("ascii"))
        counter += 1
    return out[:size].decode("ascii")


def caption_for(labels: list[int]) -> str:
    if not labels:
        return "youtube8m labels: none"
    return "youtube8m labels: " + ",".join(str(label) for label in labels)


def collect_partition(
    shards: Sequence[Path],
    count: int,
    embeddings_path: Path,
    ids_path: Path,
    metadata_jsonl: Path | None,
    start_row_id: int = 0,
) -> tuple[int, list[dict[str, Any]]]:
    embeddings_path.parent.mkdir(parents=True, exist_ok=True)
    ids_path.parent.mkdir(parents=True, exist_ok=True)
    if metadata_jsonl is not None and metadata_jsonl.exists():
        metadata_jsonl.unlink()

    embeddings = open_memmap(
        embeddings_path, mode="w+", dtype=np.float32, shape=(count, EMBED_DIM)
    )
    ids = np.empty((count,), dtype="<U64")
    rows: list[dict[str, Any]] = []
    written = 0
    for record in iter_records(shards):
        if written >= count:
            break
        row_id = start_row_id + written
        embeddings[written] = record.embedding
        ids[written] = record.video_id
        row = {
            "row_id": int(row_id),
            "video_id": record.video_id,
            "labels": record.labels,
            "frame_count": record.frame_count,
            "rgb_present": record.rgb_present,
            "audio_present": record.audio_present,
            "source_shard": record.source_shard,
            "caption": caption_for(record.labels),
            "payload_text": (
                f"YouTube-8M anonymous video {record.video_id}; "
                f"{caption_for(record.labels)}; frames={record.frame_count}"
            ),
            "synthetic_blob_256b": synthetic_blob(row_id, 256),
        }
        rows.append(row)
        if metadata_jsonl is not None:
            append_jsonl(metadata_jsonl, row)
        written += 1
    embeddings.flush()
    np.save(ids_path, ids)
    if written < count:
        raise RuntimeError(
            f"Only parsed {written} records from {len(shards)} shards; requested {count}"
        )
    return written, rows


def write_videos_parquet(rows: list[dict[str, Any]]) -> None:
    cleaned_dir().mkdir(parents=True, exist_ok=True)
    table = pa.table(
        {
            "row_id": [int(row["row_id"]) for row in rows],
            "video_id": [str(row["video_id"]) for row in rows],
            "labels": [list(map(int, row["labels"])) for row in rows],
            "frame_count": [int(row["frame_count"]) for row in rows],
            "source_shard": [str(row["source_shard"]) for row in rows],
            "caption": [str(row["caption"]) for row in rows],
            "payload_text": [str(row["payload_text"]) for row in rows],
            "synthetic_blob_256b": [str(row["synthetic_blob_256b"]) for row in rows],
        }
    )
    pq.write_table(table, cleaned_dir() / "videos.parquet")


def parse_features(args: argparse.Namespace) -> None:
    ensure_dirs()
    train_shards = expand_patterns(args.train_shards, raw_shards_dir() / "train")
    validate_shards = expand_patterns(args.validate_shards, raw_shards_dir() / "validate")
    if not train_shards:
        raise FileNotFoundError(
            f"No train TFRecord shards found under {raw_shards_dir() / 'train'}"
        )

    query_source = "validate"
    train_needed = args.target_videos
    if not validate_shards and args.allow_train_holdout:
        query_source = "train_holdout"
        train_needed = args.target_videos + args.target_queries
    elif not validate_shards:
        raise FileNotFoundError(
            f"No validate TFRecord shards found under {raw_shards_dir() / 'validate'}; "
            "use --allow-train-holdout to derive queries from train."
        )

    if query_source == "validate":
        parsed_train_records, base_rows = collect_partition(
            train_shards,
            args.target_videos,
            embeddings_dir() / "video_embeddings.npy",
            embeddings_dir() / "video_ids.npy",
            raw_dir() / "metadata.jsonl",
            start_row_id=0,
        )
        np.save(
            embeddings_dir() / "row_ids.npy",
            np.arange(args.target_videos, dtype=np.int64),
        )
        query_count, query_rows = collect_partition(
            validate_shards,
            args.target_queries,
            embeddings_dir() / "query_embeddings.npy",
            embeddings_dir() / "query_video_ids.npy",
            raw_dir() / "query_metadata.jsonl",
            start_row_id=0,
        )
        query_indices: list[int] = []
    else:
        parsed_train_records, all_rows = collect_partition(
            train_shards,
            train_needed,
            embeddings_dir() / "train_all_embeddings.npy",
            embeddings_dir() / "train_all_video_ids.npy",
            None,
            start_row_id=0,
        )
        train_all = np.load(embeddings_dir() / "train_all_embeddings.npy", mmap_mode="r")
        train_ids = np.load(embeddings_dir() / "train_all_video_ids.npy", mmap_mode="r")
        np.save(embeddings_dir() / "video_embeddings.npy", np.asarray(train_all[: args.target_videos]))
        np.save(embeddings_dir() / "video_ids.npy", np.asarray(train_ids[: args.target_videos]))
        np.save(
            embeddings_dir() / "row_ids.npy",
            np.arange(args.target_videos, dtype=np.int64),
        )
        base_rows = all_rows[: args.target_videos]
        query_count = args.target_queries
        query_rows = all_rows[args.target_videos: args.target_videos + args.target_queries]
        np.save(
            embeddings_dir() / "query_embeddings.npy",
            np.asarray(train_all[args.target_videos: args.target_videos + args.target_queries]),
        )
        np.save(
            embeddings_dir() / "query_video_ids.npy",
            np.asarray(train_ids[args.target_videos: args.target_videos + args.target_queries]),
        )
        with (raw_dir() / "metadata.jsonl").open("w", encoding="utf-8") as handle:
            for row in base_rows:
                handle.write(json.dumps(row, ensure_ascii=False) + "\n")
        with (raw_dir() / "query_metadata.jsonl").open("w", encoding="utf-8") as handle:
            for row in query_rows:
                handle.write(json.dumps(row, ensure_ascii=False) + "\n")
        query_indices = list(range(args.target_videos, args.target_videos + args.target_queries))

    write_videos_parquet(base_rows)
    split = {
        "dataset": DATASET,
        "seed": args.seed,
        "base_count": args.target_videos,
        "query_count": query_count,
        "embedding_dim": EMBED_DIM,
        "query_source": query_source,
        "query_indices": query_indices,
        "train_shards": [str(path) for path in train_shards],
        "validate_shards": [str(path) for path in validate_shards],
        "created_at": now(),
    }
    write_json(splits_dir() / "split_v1.json", split)
    update_embedding_manifest(
        {
            "embedding_method": "mean_pool_official_frame_features",
            "feature_source": "YouTube-8M official 2/frame TFRecord features",
            "rgb_dim": RGB_DIM,
            "audio_dim": AUDIO_DIM,
            "embedding_dim": EMBED_DIM,
            "normalization": "l2",
            "base_embeddings": str(embeddings_dir() / "video_embeddings.npy"),
            "query_embeddings": str(embeddings_dir() / "query_embeddings.npy"),
            "base_count": args.target_videos,
            "query_count": query_count,
            "query_source": query_source,
            "parsed_train_records": parsed_train_records,
        }
    )


def build_groundtruth(args: argparse.Namespace) -> None:
    base = np.load(embeddings_dir() / "video_embeddings.npy", mmap_mode="r")
    queries = np.load(embeddings_dir() / "query_embeddings.npy", mmap_mode="r")
    if base.ndim != 2 or queries.ndim != 2:
        raise RuntimeError("Expected 2-D base/query embeddings")
    if base.shape[1] != queries.shape[1]:
        raise RuntimeError(f"Embedding dims differ: base={base.shape}, query={queries.shape}")
    if base.shape[1] != EMBED_DIM:
        raise RuntimeError(f"Expected dim {EMBED_DIM}, got {base.shape[1]}")

    query_count = min(args.target_queries, queries.shape[0]) if args.target_queries else queries.shape[0]
    k = min(args.topk, base.shape[0])
    gt = np.empty((query_count, k), dtype=np.int64)
    gt_scores = np.empty((query_count, k), dtype=np.float32)

    for q_start in range(0, query_count, args.query_block):
        q_end = min(query_count, q_start + args.query_block)
        q_block = np.asarray(queries[q_start:q_end], dtype=np.float32)
        block_scores = np.full((q_end - q_start, k), -np.inf, dtype=np.float32)
        block_ids = np.full((q_end - q_start, k), -1, dtype=np.int64)

        for b_start in range(0, base.shape[0], args.base_block):
            b_end = min(base.shape[0], b_start + args.base_block)
            b_block = np.asarray(base[b_start:b_end], dtype=np.float32)
            scores = q_block @ b_block.T
            local_k = min(k, scores.shape[1])
            local_unsorted = np.argpartition(-scores, kth=local_k - 1, axis=1)[:, :local_k]
            local_scores = np.take_along_axis(scores, local_unsorted, axis=1)
            local_ids = local_unsorted.astype(np.int64) + b_start

            merged_scores = np.concatenate([block_scores, local_scores], axis=1)
            merged_ids = np.concatenate([block_ids, local_ids], axis=1)
            keep = np.argpartition(-merged_scores, kth=k - 1, axis=1)[:, :k]
            block_scores = np.take_along_axis(merged_scores, keep, axis=1)
            block_ids = np.take_along_axis(merged_ids, keep, axis=1)

        order = np.argsort(-block_scores, axis=1)
        gt[q_start:q_end] = np.take_along_axis(block_ids, order, axis=1)
        gt_scores[q_start:q_end] = np.take_along_axis(block_scores, order, axis=1)
        print(f"groundtruth {q_end}/{query_count}", flush=True)

    gt_dir().mkdir(parents=True, exist_ok=True)
    np.save(gt_dir() / "gt_top10.npy", gt[:, : min(10, k)])
    np.save(gt_dir() / "gt_top20.npy", gt[:, : min(20, k)])
    np.save(gt_dir() / "gt_scores_top20.npy", gt_scores[:, : min(20, k)])
    summary = {
        "dataset": DATASET,
        "base_shape": list(base.shape),
        "query_shape": [query_count, int(queries.shape[1])],
        "topk": k,
        "gt_top10": str(gt_dir() / "gt_top10.npy"),
        "gt_top20": str(gt_dir() / "gt_top20.npy"),
        "metric": "inner_product_on_l2_normalized_embeddings",
        "base_norm_sample_mean": float(np.linalg.norm(np.asarray(base[: min(1024, base.shape[0])]), axis=1).mean()),
        "query_norm_sample_mean": float(np.linalg.norm(np.asarray(queries[: min(1024, queries.shape[0])]), axis=1).mean()),
        "has_nan": bool(
            np.isnan(np.asarray(base[: min(1024, base.shape[0])])).any()
            or np.isnan(np.asarray(queries[: min(1024, queries.shape[0])])).any()
        ),
        "created_at": now(),
    }
    write_json(gt_dir() / "gt_summary.json", summary)


def _replace_path(dst: Path) -> None:
    if dst.exists() or dst.is_symlink():
        if dst.is_dir() and not dst.is_symlink():
            raise RuntimeError(f"Refusing to replace directory: {dst}")
        dst.unlink()


def _link_or_copy(src: Path, dst: Path, prefer_symlink: bool) -> str:
    dst.parent.mkdir(parents=True, exist_ok=True)
    _replace_path(dst)
    if prefer_symlink:
        try:
            os.symlink(src, dst)
            return "symlink"
        except OSError:
            pass
    shutil.copy2(src, dst)
    return "copy"


def build_adapter(args: argparse.Namespace) -> None:
    out_dir = Path(args.output_dir) if args.output_dir else adapter_default_dir()
    out_dir.mkdir(parents=True, exist_ok=True)
    prefer_symlink = bool(args.prefer_symlink)
    files = {
        "image_embeddings.npy": _link_or_copy(
            embeddings_dir() / "video_embeddings.npy",
            out_dir / "image_embeddings.npy",
            prefer_symlink,
        ),
        "query_embeddings.npy": _link_or_copy(
            embeddings_dir() / "query_embeddings.npy",
            out_dir / "query_embeddings.npy",
            prefer_symlink,
        ),
        "metadata.jsonl": _link_or_copy(
            raw_dir() / "metadata.jsonl", out_dir / "metadata.jsonl", prefer_symlink
        ),
    }
    row_ids = np.load(embeddings_dir() / "row_ids.npy", mmap_mode="r")
    np.save(out_dir / "image_ids.npy", np.asarray(row_ids, dtype=np.int64))
    query_ids_path = embeddings_dir() / "query_video_ids.npy"
    if query_ids_path.exists():
        query_video_ids = np.load(query_ids_path, mmap_mode="r")
        np.save(out_dir / "query_ids.npy", np.arange(query_video_ids.shape[0], dtype=np.int64))
    gt_path = gt_dir() / "gt_top10.npy"
    if gt_path.exists():
        files["gt_top10.npy"] = _link_or_copy(gt_path, out_dir / "gt_top10.npy", prefer_symlink)
    manifest = {
        "dataset": DATASET,
        "output_dir": str(out_dir),
        "created_at": now(),
        "id_policy": "image_ids.npy uses numeric row_id; original YouTube-8M anonymous ids remain in video_ids.npy and videos.parquet",
        "files": files,
    }
    write_json(out_dir / "adapter_manifest.json", manifest)
    print(json.dumps(manifest, ensure_ascii=False, indent=2))


def validate_outputs(args: argparse.Namespace) -> None:
    errors: list[str] = []
    base_path = embeddings_dir() / "video_embeddings.npy"
    query_path = embeddings_dir() / "query_embeddings.npy"
    parquet_path = cleaned_dir() / "videos.parquet"
    gt10_path = gt_dir() / "gt_top10.npy"
    gt20_path = gt_dir() / "gt_top20.npy"

    for path in [base_path, query_path, parquet_path, gt10_path, gt20_path]:
        if not path.exists():
            errors.append(f"missing {path}")
    if errors:
        raise RuntimeError("; ".join(errors))

    base = np.load(base_path, mmap_mode="r")
    queries = np.load(query_path, mmap_mode="r")
    gt10 = np.load(gt10_path, mmap_mode="r")
    gt20 = np.load(gt20_path, mmap_mode="r")
    videos = pq.ParquetFile(parquet_path)
    if base.ndim != 2 or base.shape[1] != EMBED_DIM:
        errors.append(f"bad base shape {base.shape}")
    if queries.ndim != 2 or queries.shape[1] != EMBED_DIM:
        errors.append(f"bad query shape {queries.shape}")
    if videos.metadata is None or videos.metadata.num_rows != base.shape[0]:
        errors.append("videos.parquet row count mismatch")
    if gt10.shape != (queries.shape[0], min(10, base.shape[0])):
        errors.append(f"bad gt_top10 shape {gt10.shape}")
    if gt20.shape != (queries.shape[0], min(20, base.shape[0])):
        errors.append(f"bad gt_top20 shape {gt20.shape}")
    sample_base = np.asarray(base[: min(1024, base.shape[0])])
    sample_queries = np.asarray(queries[: min(1024, queries.shape[0])])
    if np.isnan(sample_base).any() or np.isnan(sample_queries).any():
        errors.append("nan in embedding sample")
    if not np.allclose(np.linalg.norm(sample_base, axis=1), 1.0, atol=1e-2):
        errors.append("base embeddings are not unit-normalized")
    if not np.allclose(np.linalg.norm(sample_queries, axis=1), 1.0, atol=1e-2):
        errors.append("query embeddings are not unit-normalized")
    if args.exact_check_queries > 0:
        n = min(args.exact_check_queries, queries.shape[0])
        for idx in range(n):
            scores = np.asarray(base) @ np.asarray(queries[idx], dtype=np.float32)
            exact = np.argsort(-scores)[: min(10, base.shape[0])]
            if not np.array_equal(exact, np.asarray(gt10[idx, : exact.shape[0]])):
                errors.append(f"exact check failed for query {idx}")
                break
    if errors:
        raise RuntimeError("; ".join(errors))
    print(
        json.dumps(
            {
                "dataset": DATASET,
                "base_shape": list(base.shape),
                "query_shape": list(queries.shape),
                "videos_rows": videos.metadata.num_rows if videos.metadata else None,
                "gt_top10_shape": list(gt10.shape),
                "gt_top20_shape": list(gt20.shape),
                "status": "ok",
            },
            ensure_ascii=False,
            indent=2,
        )
    )


def add_common_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--target-videos", type=int, default=1_000_000)
    parser.add_argument("--target-queries", type=int, default=10_000)
    parser.add_argument("--seed", type=int, default=DEFAULT_SEED)
    parser.add_argument("--train-shards", nargs="*", default=[])
    parser.add_argument("--validate-shards", nargs="*", default=[])
    parser.set_defaults(allow_train_holdout=True)
    parser.add_argument(
        "--allow-train-holdout",
        dest="allow_train_holdout",
        action="store_true",
        help="Allow deterministic train holdout queries when validation shards are absent.",
    )
    parser.add_argument(
        "--no-train-holdout",
        dest="allow_train_holdout",
        action="store_false",
        help="Fail instead of deriving queries from train when validation shards are absent.",
    )


def add_download_args(parser: argparse.ArgumentParser) -> None:
    parser.add_argument("--skip-download", action="store_true")
    parser.add_argument("--mirrors", default=",".join(DEFAULT_MIRRORS))
    parser.add_argument("--train-shard-denominator", type=int, default=100)
    parser.add_argument("--train-shard-count", type=int, default=25)
    parser.add_argument("--validate-shard-denominator", type=int, default=100)
    parser.add_argument("--validate-shard-count", type=int, default=2)
    parser.add_argument("--train-shard-specs", nargs="*", default=[])
    parser.add_argument("--validate-shard-specs", nargs="*", default=[])


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="command", required=True)

    p_check = sub.add_parser("check-deps", help="Check local dependencies.")

    p_download = sub.add_parser("download", help="Download official YouTube-8M shards.")
    add_common_args(p_download)
    add_download_args(p_download)

    p_parse = sub.add_parser("parse", help="Parse TFRecord shards into embeddings/payload.")
    add_common_args(p_parse)

    p_gt = sub.add_parser("groundtruth", help="Build exact inner-product top-k.")
    p_gt.add_argument("--target-queries", type=int, default=0)
    p_gt.add_argument("--topk", type=int, default=TOPK)
    p_gt.add_argument("--query-block", type=int, default=64)
    p_gt.add_argument("--base-block", type=int, default=200_000)

    p_adapter = sub.add_parser("adapter", help="Build bench_e2e COCO-style adapter.")
    p_adapter.add_argument("--output-dir", default="")
    p_adapter.add_argument("--prefer-symlink", action="store_true")

    p_validate = sub.add_parser("validate", help="Validate generated dataset artifacts.")
    p_validate.add_argument("--exact-check-queries", type=int, default=0)

    p_all = sub.add_parser("all", help="Download, parse, build GT, adapter, validate.")
    add_common_args(p_all)
    add_download_args(p_all)
    p_all.add_argument("--topk", type=int, default=TOPK)
    p_all.add_argument("--query-block", type=int, default=64)
    p_all.add_argument("--base-block", type=int, default=200_000)
    p_all.add_argument("--output-dir", default="")
    p_all.add_argument("--prefer-symlink", action="store_true")
    p_all.add_argument("--exact-check-queries", type=int, default=0)

    args = parser.parse_args()
    if args.command == "check-deps":
        versions = dependency_versions()
        print(json.dumps(versions, ensure_ascii=False, indent=2))
        if versions["tensorflow"].startswith("missing"):
            return 2
        return 0
    if args.command == "download":
        maybe_download(args)
        return 0
    if args.command == "parse":
        parse_features(args)
        return 0
    if args.command == "groundtruth":
        build_groundtruth(args)
        return 0
    if args.command == "adapter":
        build_adapter(args)
        return 0
    if args.command == "validate":
        validate_outputs(args)
        return 0
    if args.command == "all":
        maybe_download(args)
        parse_features(args)
        build_groundtruth(args)
        build_adapter(args)
        validate_outputs(args)
        return 0
    raise AssertionError(args.command)


if __name__ == "__main__":
    raise SystemExit(main())
