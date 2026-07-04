#!/usr/bin/env python3
"""Estimate lossless compression opportunities for v14 direct3 bitplane .clu files."""

from __future__ import annotations

import argparse
import csv
import struct
from collections import Counter
from pathlib import Path


GLOBAL_MAGIC = 0x4C4D4356
BLOCK_MAGIC = 0x424C4356
ADDRESS_FORMAT_V2 = 2


def read_u32(buf: bytes, off: int) -> tuple[int, int]:
    return struct.unpack_from("<I", buf, off)[0], off + 4


def parse_header(data: bytes) -> dict:
    off = 0
    magic, off = read_u32(data, off)
    if magic != GLOBAL_MAGIC:
        raise ValueError("bad global magic")
    version, off = read_u32(data, off)
    num_clusters, off = read_u32(data, off)
    dim, off = read_u32(data, off)
    bits = data[off]
    off += 1
    block_size, off = read_u32(data, off)
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
    path_len, off = read_u32(data, off)
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
        cluster_id, off = read_u32(data, off)
        num_records, off = read_u32(data, off)
        epsilon = struct.unpack_from("<f", data, off)[0]
        off += 4
        off += dim * 4
        block_offset = struct.unpack_from("<Q", data, off)[0]
        off += 8
        block_size = struct.unpack_from("<Q", data, off)[0]
        off += 8
        num_fastscan_blocks, off = read_u32(data, off)
        exrabitq_region_offset, off = read_u32(data, off)
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
    expected = header["lookup_offset"] + entry_size * header["num_clusters"]
    if off != expected:
        raise AssertionError("lookup parser drift")
    return entries


def parse_address_payload_offset(block: bytes) -> tuple[int, int, int]:
    if len(block) < 8:
        raise ValueError("block too small")
    mini_trailer_size, magic = struct.unpack_from("<II", block, len(block) - 8)
    if magic != BLOCK_MAGIC:
        raise ValueError("bad block magic")
    trailer_off = len(block) - mini_trailer_size
    vals = struct.unpack_from("<IIIIII", block, trailer_off)
    address_format, _page_size, entry_size, num_entries, payload_off, payload_bytes = vals
    if address_format != ADDRESS_FORMAT_V2 or entry_size == 0:
        raise ValueError("unexpected address trailer")
    return payload_off, payload_bytes, num_entries


def analyze(data: bytes, header: dict, entries: list[dict]) -> dict:
    dim = header["dim"]
    ex_bits = header["ex_bits"]
    if header["version"] < 14 or ex_bits != 3:
        raise ValueError("expected v14 official direct ex_bits=3")
    dim_block = 64
    batch = 8
    num_dim_blocks = (dim + dim_block - 1) // dim_block
    bytes_per_lane = (dim_block * ex_bits + 7) // 8
    fixed_payload_per_db = batch * bytes_per_lane
    fixed_batch_block = 4 + num_dim_blocks * fixed_payload_per_db + batch * 2 * 4
    fastscan_block_size = dim * 4 + 32 * 4

    total_records = 0
    total_batch_blocks = 0
    fixed_ex_bytes = 0
    actual_ex_bytes = 0
    sparse_ex_bytes = 0
    const_ex_bytes = 0
    zero_words = 0
    one_words = 0
    mixed_words = 0
    total_words = 0
    plane_zero = [0, 0, 0]
    plane_one = [0, 0, 0]
    plane_words = [0, 0, 0]
    plane_popcounts = [Counter(), Counter(), Counter()]
    value_hist = Counter()
    valid_lane_words = 0
    valid_lane_bytes = 0
    trimmed_ex_bytes = 0
    sparse_positions_ex_bytes = 0

    for entry in entries:
        if entry["num_records"] == 0:
            continue
        block = data[entry["block_offset"] : entry["block_offset"] + entry["block_size"]]
        address_payload_off, _address_payload_bytes, num_entries = parse_address_payload_offset(block)
        if num_entries != entry["num_records"]:
            raise ValueError("address entry count mismatch")
        num_batch_blocks = (entry["num_records"] + batch - 1) // batch
        region1_size = entry["num_fastscan_blocks"] * fastscan_block_size
        ex_off = region1_size
        ex_bytes = address_payload_off - ex_off
        total_records += entry["num_records"]
        total_batch_blocks += num_batch_blocks
        fixed_ex_bytes += num_batch_blocks * fixed_batch_block
        actual_ex_bytes += ex_bytes

        for bb in range(num_batch_blocks):
            block_off = ex_off + bb * fixed_batch_block
            valid = struct.unpack_from("<I", block, block_off)[0]
            payload = block_off + 4
            if valid > batch:
                raise ValueError("bad valid_count")
            # Scheme A: per dim-block and plane, store one lane-present mask byte,
            # then only non-zero 64-bit lane words.
            # Scheme B: additionally elide all-one words via one-mask.
            sparse_size = 4 + batch * 2 * 4
            const_size = 4 + batch * 2 * 4
            trimmed_size = 4 + valid * 2 * 4
            sparse_positions_size = 4 + valid * 2 * 4
            for db in range(num_dim_blocks):
                sparse_size += ex_bits
                const_size += ex_bits * 2
                trimmed_size += valid * bytes_per_lane
                # Per plane: one coding-mode byte per valid lane. Mode 0=zero,
                # mode 1=raw 64-bit word, mode 2=sparse positions.
                sparse_positions_size += ex_bits * valid
                db_base = payload + db * fixed_payload_per_db
                for plane in range(ex_bits):
                    nonzero_count = 0
                    mixed_count = 0
                    for lane in range(batch):
                        lane_base = db_base + lane * bytes_per_lane
                        word = struct.unpack_from("<Q", block, lane_base + plane * 8)[0]
                        total_words += 1
                        plane_words[plane] += 1
                        if word == 0:
                            zero_words += 1
                            plane_zero[plane] += 1
                        else:
                            nonzero_count += 1
                            if word == 0xFFFFFFFFFFFFFFFF:
                                one_words += 1
                                plane_one[plane] += 1
                            else:
                                mixed_words += 1
                                mixed_count += 1
                        if lane < valid:
                            valid_lane_words += 1
                            pc = word.bit_count()
                            plane_popcounts[plane][pc] += 1
                            # Sparse positions are smaller than raw when 1 + popcount < 8.
                            if pc == 0:
                                pass
                            elif pc <= 6:
                                sparse_positions_size += 1 + pc
                            else:
                                sparse_positions_size += 8
                    sparse_size += nonzero_count * 8
                    const_size += mixed_count * 8
                for lane in range(valid):
                    lane_base = db_base + lane * bytes_per_lane
                    w0 = struct.unpack_from("<Q", block, lane_base)[0]
                    w1 = struct.unpack_from("<Q", block, lane_base + 8)[0]
                    w2 = struct.unpack_from("<Q", block, lane_base + 16)[0]
                    for bit in range(64):
                        v = ((w0 >> bit) & 1) | (((w1 >> bit) & 1) << 1) | (((w2 >> bit) & 1) << 2)
                        value_hist[v] += 1
            sparse_ex_bytes += sparse_size
            const_ex_bytes += const_size
            trimmed_ex_bytes += trimmed_size
            sparse_positions_ex_bytes += sparse_positions_size
            valid_lane_bytes += valid * num_dim_blocks * bytes_per_lane + valid * 2 * 4 + 4

    return {
        "version": header["version"],
        "dim": dim,
        "total_bits": header["total_bits"],
        "ex_bits": ex_bits,
        "records": total_records,
        "clusters": len(entries),
        "batch_blocks": total_batch_blocks,
        "fixed_ex_bytes": fixed_ex_bytes,
        "actual_ex_bytes": actual_ex_bytes,
        "sparse_ex_bytes": sparse_ex_bytes,
        "const_ex_bytes": const_ex_bytes,
        "trimmed_ex_bytes": trimmed_ex_bytes,
        "sparse_positions_ex_bytes": sparse_positions_ex_bytes,
        "sparse_ratio_vs_fixed": sparse_ex_bytes / fixed_ex_bytes if fixed_ex_bytes else 0,
        "const_ratio_vs_fixed": const_ex_bytes / fixed_ex_bytes if fixed_ex_bytes else 0,
        "trimmed_ratio_vs_fixed": trimmed_ex_bytes / fixed_ex_bytes if fixed_ex_bytes else 0,
        "sparse_positions_ratio_vs_fixed": (
            sparse_positions_ex_bytes / fixed_ex_bytes if fixed_ex_bytes else 0
        ),
        "zero_words": zero_words,
        "one_words": one_words,
        "mixed_words": mixed_words,
        "total_words": total_words,
        "zero_word_rate": zero_words / total_words if total_words else 0,
        "one_word_rate": one_words / total_words if total_words else 0,
        "mixed_word_rate": mixed_words / total_words if total_words else 0,
        "plane_zero_rates": [
            plane_zero[i] / plane_words[i] if plane_words[i] else 0 for i in range(3)
        ],
        "plane_one_rates": [
            plane_one[i] / plane_words[i] if plane_words[i] else 0 for i in range(3)
        ],
        "value_hist": dict(sorted(value_hist.items())),
        "plane_popcount_hist": [dict(sorted(c.items())) for c in plane_popcounts],
    }


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--clu", required=True, type=Path)
    ap.add_argument("--out-csv", type=Path)
    args = ap.parse_args()
    data = args.clu.read_bytes()
    header = parse_header(data)
    entries = parse_lookup(data, header)
    stats = analyze(data, header, entries)
    for key, value in stats.items():
        print(f"{key}: {value}")
    if args.out_csv:
        args.out_csv.parent.mkdir(parents=True, exist_ok=True)
        with args.out_csv.open("w", newline="") as f:
            writer = csv.DictWriter(f, fieldnames=list(stats.keys()))
            writer.writeheader()
            writer.writerow(stats)


if __name__ == "__main__":
    main()
