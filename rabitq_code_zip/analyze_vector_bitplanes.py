#!/usr/bin/env python3
"""Analyze v15 vector_bitplanes ExData bitplane sparsity."""

from __future__ import annotations

import argparse
import csv
import json
import lzma
import math
import random
import struct
import time
import zlib
from collections import Counter
from pathlib import Path

try:
    import lz4.frame as lz4_frame
except ImportError:  # pragma: no cover - optional local codec.
    lz4_frame = None

try:
    import zstandard as zstd
except ImportError:  # pragma: no cover - optional local codec.
    zstd = None


GLOBAL_MAGIC = 0x4C4D4356
BLOCK_MAGIC = 0x424C4356
VARIABLE_EX_MAGIC = 0x315A5845  # EXZ1
LAYOUT_SPLIT3_TRIMMED = 6
LAYOUT_SPLIT3_ZERO_ELIDE = 7
LAYOUT_VECTOR_BITPLANES = 8
LAYOUT_VECTOR_BITPLANES_PREFETCH = 9
LAYOUT_VECTOR_NIBBLE4 = 10
LAYOUT_VECTOR_2BIT = 11
LAYOUT_SMALL_LANE4 = 12
LAYOUT_SMALL_LANE2 = 13
LAYOUT_VECTOR_BITPLANES_MICROBATCH = 14


def layout_name(layout: int) -> str:
    return {
        LAYOUT_SPLIT3_TRIMMED: "split3_trimmed_bitplanes",
        LAYOUT_SPLIT3_ZERO_ELIDE: "split3_zero_plane_elide",
        LAYOUT_VECTOR_BITPLANES: "vector_bitplanes",
        LAYOUT_VECTOR_BITPLANES_PREFETCH: "vector_bitplanes_prefetch",
        LAYOUT_VECTOR_NIBBLE4: "vector_nibble4",
        LAYOUT_VECTOR_2BIT: "vector_2bit",
        LAYOUT_SMALL_LANE4: "small_lane4_bitplanes",
        LAYOUT_SMALL_LANE2: "small_lane2_bitplanes",
        LAYOUT_VECTOR_BITPLANES_MICROBATCH: "vector_bitplanes_microbatch",
    }.get(layout, f"layout_{layout}")


def u32(buf: bytes, off: int) -> tuple[int, int]:
    return struct.unpack_from("<I", buf, off)[0], off + 4


def parse_header(data: bytes) -> dict:
    off = 0
    magic, off = u32(data, off)
    if magic != GLOBAL_MAGIC:
        raise ValueError("bad global magic")
    version, off = u32(data, off)
    num_clusters, off = u32(data, off)
    dim, off = u32(data, off)
    bits = data[off]
    off += 1
    block_size, off = u32(data, off)
    c_factor = struct.unpack_from("<f", data, off)[0]
    off += 4
    total_bits = bits
    ex_bits = bits if bits > 1 else 0
    estimator_mode = 0
    exdata_layout = 0
    if version >= 13:
        total_bits = data[off]
        off += 1
        ex_bits = data[off]
        off += 1
        estimator_mode = data[off]
        off += 1
        if version >= 14:
            exdata_layout = data[off]
            off += 1
    path_len, off = u32(data, off)
    data_path = data[off : off + 256].split(b"\0", 1)[0].decode("utf-8", "ignore")
    off += 256
    return {
        "version": version,
        "num_clusters": num_clusters,
        "dim": dim,
        "bits": bits,
        "block_size": block_size,
        "c_factor": c_factor,
        "total_bits": total_bits,
        "ex_bits": ex_bits,
        "estimator_mode": estimator_mode,
        "exdata_layout": exdata_layout,
        "data_path": data_path[:path_len] if path_len else data_path,
        "lookup_offset": off,
    }


def parse_lookup(data: bytes, header: dict) -> list[dict]:
    dim = header["dim"]
    entry_size = 4 + 4 + 4 + dim * 4 + 8 + 8 + 4 + 4
    off = header["lookup_offset"]
    entries = []
    for _ in range(header["num_clusters"]):
        cluster_id, off = u32(data, off)
        num_records, off = u32(data, off)
        epsilon = struct.unpack_from("<f", data, off)[0]
        off += 4 + dim * 4
        block_offset = struct.unpack_from("<Q", data, off)[0]
        off += 8
        block_size = struct.unpack_from("<Q", data, off)[0]
        off += 8
        num_fastscan_blocks, off = u32(data, off)
        exrabitq_region_offset, off = u32(data, off)
        entries.append(
            {
                "cluster_id": cluster_id,
                "num_records": num_records,
                "epsilon": epsilon,
                "block_offset": block_offset,
                "block_size": block_size,
                "num_fastscan_blocks": num_fastscan_blocks,
                "exrabitq_region_offset": exrabitq_region_offset,
            }
        )
    if off != header["lookup_offset"] + entry_size * header["num_clusters"]:
        raise ValueError("lookup parser drift")
    return entries


def parse_address_payload_offset(block: bytes) -> tuple[int, int, int]:
    mini_trailer_size, magic = struct.unpack_from("<II", block, len(block) - 8)
    if magic != BLOCK_MAGIC:
        raise ValueError("bad block magic")
    trailer_off = len(block) - mini_trailer_size
    _address_format, _page_size, entry_size, num_entries, payload_off, payload_bytes = (
        struct.unpack_from("<IIIIII", block, trailer_off)
    )
    if entry_size == 0:
        raise ValueError("bad address trailer")
    return payload_off, payload_bytes, num_entries


def compact_offset(
    layout: int,
    lane: int,
    db: int,
    valid: int,
    num_dim_blocks: int,
    bytes_per_lane: int,
) -> int:
    if layout in (
        LAYOUT_VECTOR_BITPLANES,
        LAYOUT_VECTOR_BITPLANES_PREFETCH,
        LAYOUT_VECTOR_BITPLANES_MICROBATCH,
        LAYOUT_VECTOR_NIBBLE4,
        LAYOUT_VECTOR_2BIT,
    ):
        return (lane * num_dim_blocks + db) * bytes_per_lane
    if layout == LAYOUT_SMALL_LANE4 or layout == LAYOUT_SMALL_LANE2:
        group_size = 2 if layout == LAYOUT_SMALL_LANE2 else 4
        group_start = (lane // group_size) * group_size
        group_lanes = min(group_size, valid - group_start)
        local_lane = lane - group_start
        preceding = group_start * num_dim_blocks * bytes_per_lane
        return preceding + (db * group_lanes + local_lane) * bytes_per_lane
    if layout == LAYOUT_SPLIT3_TRIMMED:
        return (db * valid + lane) * bytes_per_lane
    raise ValueError(f"unsupported layout for fixed payload parsing: {layout_name(layout)}")


def bitplane_words_from_direct(compact: bytes, ex_bits: int) -> list[int]:
    return [struct.unpack_from("<Q", compact, plane * 8)[0] for plane in range(ex_bits)]


def bitplane_words_from_nibble4(compact: bytes) -> list[int]:
    words = [0, 0, 0, 0]
    for group in range(4):
        base = group * 8
        dim_base = group * 16
        for j in range(8):
            byte = compact[base + j]
            lo = byte & 0x0F
            hi = (byte >> 4) & 0x0F
            for plane in range(4):
                if (lo >> plane) & 1:
                    words[plane] |= 1 << (dim_base + j)
                if (hi >> plane) & 1:
                    words[plane] |= 1 << (dim_base + 8 + j)
    return words


def bitplane_words_from_2bit(compact: bytes) -> list[int]:
    words = [0, 0]
    for j in range(16):
        byte = compact[j]
        for sub in range(4):
            value = (byte >> (sub * 2)) & 0x03
            dim = j + sub * 16
            for plane in range(2):
                if (value >> plane) & 1:
                    words[plane] |= 1 << dim
    return words


def bitplane_words(layout: int, compact: bytes, ex_bits: int) -> list[int]:
    if layout == LAYOUT_VECTOR_NIBBLE4:
        if ex_bits != 4:
            raise ValueError("vector_nibble4 requires ex_bits=4")
        return bitplane_words_from_nibble4(compact)
    if layout == LAYOUT_VECTOR_2BIT:
        if ex_bits != 2:
            raise ValueError("vector_2bit requires ex_bits=2")
        return bitplane_words_from_2bit(compact)
    return bitplane_words_from_direct(compact, ex_bits)


def entropy_bits_per_symbol(hist: Counter) -> float:
    total = sum(hist.values())
    if total == 0:
        return 0.0
    entropy = 0.0
    for count in hist.values():
        p = count / total
        entropy -= p * math.log2(p)
    return entropy


def parse_csv_ints(value: str) -> list[int]:
    result = []
    for item in value.split(","):
        item = item.strip()
        if not item:
            continue
        result.append(int(item))
    return result


def parse_csv_strings(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def compress_chunk(algorithm: str, chunk: bytes) -> bytes:
    if algorithm.startswith("zlib"):
        level = int(algorithm[4:] or "6")
        return zlib.compress(chunk, level)
    if algorithm.startswith("lzma"):
        preset = int(algorithm[4:] or "0")
        return lzma.compress(chunk, preset=preset, check=lzma.CHECK_NONE)
    if algorithm.startswith("zstd"):
        if zstd is None:
            raise ValueError("zstandard module is not available")
        level = int(algorithm[4:] or "3")
        return zstd.ZstdCompressor(level=level).compress(chunk)
    if algorithm.startswith("lz4"):
        if lz4_frame is None:
            raise ValueError("lz4.frame module is not available")
        level = int(algorithm[3:] or "0")
        return lz4_frame.compress(
            chunk, compression_level=level, block_linked=False, store_size=True
        )
    raise ValueError(f"unsupported compression algorithm: {algorithm}")


def decompress_chunk(algorithm: str, chunk: bytes) -> bytes:
    if algorithm.startswith("zlib"):
        return zlib.decompress(chunk)
    if algorithm.startswith("lzma"):
        return lzma.decompress(chunk)
    if algorithm.startswith("zstd"):
        if zstd is None:
            raise ValueError("zstandard module is not available")
        return zstd.ZstdDecompressor().decompress(chunk)
    if algorithm.startswith("lz4"):
        if lz4_frame is None:
            raise ValueError("lz4.frame module is not available")
        return lz4_frame.decompress(chunk)
    raise ValueError(f"unsupported compression algorithm: {algorithm}")


def evaluate_block_entropy_compression(
    payload_stream: bytes,
    ex_bits: int,
    block_sizes: list[int],
    algorithms: list[str],
    sample_blocks: int,
    seed: int,
) -> list[dict]:
    if not payload_stream:
        return []
    results = []
    rng = random.Random(seed)
    payload_bytes = len(payload_stream)
    target_ex3_payload_bytes = payload_bytes * 3 / 4 if ex_bits == 4 else None
    for block_size in block_sizes:
        if block_size <= 0:
            raise ValueError("compression block sizes must be positive")
        raw_blocks = [
            payload_stream[pos : pos + block_size]
            for pos in range(0, payload_bytes, block_size)
        ]
        offset_table_bytes = (len(raw_blocks) + 1) * 8
        for algorithm in algorithms:
            t0 = time.perf_counter()
            compressed_blocks = [compress_chunk(algorithm, block) for block in raw_blocks]
            compression_ms = (time.perf_counter() - t0) * 1000.0
            compressed_payload_bytes = sum(len(block) for block in compressed_blocks)
            total_compressed_bytes = compressed_payload_bytes + offset_table_bytes
            sample_count = min(sample_blocks, len(compressed_blocks))
            sample_indices = [rng.randrange(len(compressed_blocks)) for _ in range(sample_count)]
            t1 = time.perf_counter()
            decoded_bytes = 0
            for idx in sample_indices:
                decoded = decompress_chunk(algorithm, compressed_blocks[idx])
                decoded_bytes += len(decoded)
            decompress_ms = (time.perf_counter() - t1) * 1000.0
            avg_decompress_us = (
                decompress_ms * 1000.0 / sample_count if sample_count else 0.0
            )
            throughput_mib_s = (
                decoded_bytes / (1024 * 1024) / (decompress_ms / 1000.0)
                if decompress_ms > 0.0
                else 0.0
            )
            item = {
                "algorithm": algorithm,
                "block_size": block_size,
                "num_blocks": len(raw_blocks),
                "offset_table_bytes": offset_table_bytes,
                "compressed_payload_bytes": compressed_payload_bytes,
                "total_compressed_bytes": total_compressed_bytes,
                "saving_vs_payload": 1.0 - total_compressed_bytes / payload_bytes,
                "avg_compressed_block_bytes": compressed_payload_bytes / len(raw_blocks),
                "compression_ms": compression_ms,
                "sample_blocks": sample_count,
                "avg_random_block_decompress_us": avg_decompress_us,
                "decompress_throughput_mib_s": throughput_mib_s,
            }
            if target_ex3_payload_bytes is not None:
                item["target_ex3_payload_bytes"] = target_ex3_payload_bytes
                item["ratio_to_ex3_payload"] = total_compressed_bytes / target_ex3_payload_bytes
            results.append(item)
    return results


def analyze(
    data: bytes,
    header: dict,
    entries: list[dict],
    collect_payload: bool = False,
) -> tuple[dict, bytes | None]:
    if header["version"] < 15:
        raise ValueError("analysis requires v15 variable ExData")
    dim = header["dim"]
    ex_bits = header["ex_bits"]
    if not (1 <= ex_bits <= 4):
        raise ValueError("expected ex_bits in [1, 4]")
    layout = header["exdata_layout"]
    if layout == LAYOUT_SPLIT3_ZERO_ELIDE:
        raise ValueError("split3_zero_plane_elide has variable sparse payload; unsupported")
    dim_block = 64
    batch = 8
    num_dim_blocks = (dim + dim_block - 1) // dim_block
    bytes_per_lane = (dim_block * ex_bits + 7) // 8
    fastscan_block_size = ((dim + 7) // 8) * 32 + 32 * 4

    total_records = 0
    total_blocks = 0
    value_hist = Counter()
    plane_words = [0] * ex_bits
    plane_zero = [0] * ex_bits
    plane_one = [0] * ex_bits
    plane_popcount = [Counter() for _ in range(ex_bits)]
    total_payload_bytes = 0
    total_factor_bytes = 0
    high_plane_mask_bytes = 0
    high_plane_raw_bytes = 0
    high_plane_mask_nonzero_words = 0
    payload_chunks = []

    for entry in entries:
        if entry["num_records"] == 0:
            continue
        block = data[entry["block_offset"] : entry["block_offset"] + entry["block_size"]]
        address_payload_off, _address_payload_bytes, num_entries = parse_address_payload_offset(block)
        if num_entries != entry["num_records"]:
            raise ValueError("address entry count mismatch")
        region1_size = entry["num_fastscan_blocks"] * fastscan_block_size
        region = block[region1_size:address_payload_off]
        magic, stored_blocks = struct.unpack_from("<II", region, 0)
        if magic != VARIABLE_EX_MAGIC:
            raise ValueError("bad variable ExData magic")
        offsets = list(struct.unpack_from("<" + "I" * (stored_blocks + 1), region, 8))
        if offsets[0] != 8 + (stored_blocks + 1) * 4 or offsets[-1] != len(region):
            raise ValueError("bad offset table")

        for bb in range(stored_blocks):
            bb_begin, bb_end = offsets[bb], offsets[bb + 1]
            cur = bb_begin
            valid = struct.unpack_from("<I", region, cur)[0]
            cur += 4
            if valid > batch:
                raise ValueError("bad valid_count")
            expected_payload = valid * num_dim_blocks * bytes_per_lane
            payload = region[cur : cur + expected_payload]
            cur += expected_payload
            factor_bytes = valid * 2 * 4
            if cur + factor_bytes != bb_end:
                raise ValueError(f"block size mismatch; not fixed payload layout? {layout_name(layout)}")
            if collect_payload:
                payload_chunks.append(bytes(payload))

            total_records += valid
            total_blocks += 1
            total_payload_bytes += expected_payload
            total_factor_bytes += factor_bytes + 4

            for lane in range(valid):
                for db in range(num_dim_blocks):
                    base = compact_offset(
                        layout, lane, db, valid, num_dim_blocks, bytes_per_lane
                    )
                    compact = payload[base : base + bytes_per_lane]
                    words = bitplane_words(layout, compact, ex_bits)
                    for plane, word in enumerate(words):
                        plane_words[plane] += 1
                        if word == 0:
                            plane_zero[plane] += 1
                        if word == 0xFFFFFFFFFFFFFFFF:
                            plane_one[plane] += 1
                        plane_popcount[plane][word.bit_count()] += 1
                    for bit in range(64):
                        v = 0
                        for plane, word in enumerate(words):
                            v |= ((word >> bit) & 1) << plane
                        value_hist[v] += 1

            if ex_bits == 4:
                # Per dim-block, encode the 4th bitplane across valid lanes with
                # one lane mask byte and raw nonzero 64-bit words.
                for db in range(num_dim_blocks):
                    high_plane_mask_bytes += 1
                    nonzero = 0
                    for lane in range(valid):
                        base = compact_offset(
                            layout, lane, db, valid, num_dim_blocks, bytes_per_lane
                        )
                        compact = payload[base : base + bytes_per_lane]
                        word = bitplane_words(layout, compact, ex_bits)[3]
                        if word != 0:
                            nonzero += 1
                    high_plane_mask_nonzero_words += nonzero
                    high_plane_raw_bytes += nonzero * 8

    high_plane_elide_bytes = None
    high_plane_elide_saving = None
    if ex_bits == 4:
        high_plane_elide_bytes = (
            total_payload_bytes - total_records * num_dim_blocks * 8
            + high_plane_mask_bytes
            + high_plane_raw_bytes
        )
        high_plane_elide_saving = 1.0 - high_plane_elide_bytes / total_payload_bytes

    summary = {
        "version": header["version"],
        "dim": dim,
        "total_bits": header["total_bits"],
        "ex_bits": ex_bits,
        "exdata_layout": header["exdata_layout"],
        "exdata_layout_name": layout_name(header["exdata_layout"]),
        "records": total_records,
        "batch_blocks": total_blocks,
        "num_dim_blocks": num_dim_blocks,
        "bytes_per_lane_dim_block": bytes_per_lane,
        "payload_bytes": total_payload_bytes,
        "factor_and_header_bytes": total_factor_bytes,
        "plane_zero_rates": [
            plane_zero[i] / plane_words[i] if plane_words[i] else 0 for i in range(ex_bits)
        ],
        "plane_one_rates": [
            plane_one[i] / plane_words[i] if plane_words[i] else 0 for i in range(ex_bits)
        ],
        "high_plane_mask_nonzero_words": high_plane_mask_nonzero_words,
        "high_plane_elide_payload_bytes": high_plane_elide_bytes,
        "high_plane_elide_payload_saving": high_plane_elide_saving,
        "value_entropy_bits_per_dim": entropy_bits_per_symbol(value_hist),
        "value_hist": dict(sorted(value_hist.items())),
        "plane_popcount": [
            {str(k): v for k, v in sorted(counter.items())} for counter in plane_popcount
        ],
    }
    return summary, b"".join(payload_chunks) if collect_payload else None


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("cluster_clu", type=Path)
    ap.add_argument("--json-out", type=Path)
    ap.add_argument("--csv-out", type=Path)
    ap.add_argument(
        "--compression-algorithms",
        default="",
        help=(
            "Comma-separated codecs to evaluate, e.g. "
            "zlib1,zlib6,lzma0,zstd3,lz4."
        ),
    )
    ap.add_argument(
        "--compression-block-sizes",
        default="4096,16384,65536",
        help="Comma-separated independent compression block sizes in bytes.",
    )
    ap.add_argument("--compression-sample-blocks", type=int, default=2048)
    ap.add_argument("--compression-seed", type=int, default=42)
    args = ap.parse_args()

    data = args.cluster_clu.read_bytes()
    header = parse_header(data)
    entries = parse_lookup(data, header)
    compression_algorithms = parse_csv_strings(args.compression_algorithms)
    summary, payload_stream = analyze(
        data, header, entries, collect_payload=bool(compression_algorithms)
    )
    if compression_algorithms:
        summary["block_entropy_compression"] = evaluate_block_entropy_compression(
            payload_stream or b"",
            summary["ex_bits"],
            parse_csv_ints(args.compression_block_sizes),
            compression_algorithms,
            args.compression_sample_blocks,
            args.compression_seed,
        )
    print(json.dumps(summary, indent=2, sort_keys=True))
    if args.json_out:
        args.json_out.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n")
    if args.csv_out:
        with args.csv_out.open("w", newline="") as f:
            writer = csv.writer(f)
            writer.writerow(["metric", "value"])
            for key, value in summary.items():
                if isinstance(value, (dict, list)):
                    writer.writerow([key, json.dumps(value, sort_keys=True)])
                else:
                    writer.writerow([key, value])


if __name__ == "__main__":
    main()
