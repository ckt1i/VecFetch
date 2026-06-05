## Why

The completed official RaBitQ `1+n` implementation makes bit semantics correct, but the `total_bits=4, ex_bits=3` path is much slower than expected because query-time Stage2 first decodes packed 3-bit ExData into scratch and then runs a lane-wise dot product. This change replaces that hot path with direct compact-code SIMD kernels and empirically compares two candidate 3-bit layouts before keeping the faster one.

## What Changes

- Add a rebuild-required optimized official ExData storage/query path for `ex_bits=3`.
- Implement a direct compact IP kernel for the official-library-style `2-bit + 1-bit` layout.
- Implement a direct compact IP kernel for a `1-bit + 1-bit + 1-bit` bitplane layout.
- Run both layouts on COCO100k with the existing `nlist=2048, nprobe=64, topk=10, non_safeout_candidate_budget=400` probe workflow.
- Select and keep the better layout based on correctness, QPS/latency, peak RSS, resident code bytes, and average rerank count.
- Preserve the existing v13 decode-to-scratch official path as a validation/fallback path while preventing it from being the selected fast path when an optimized layout passes.
- Update benchmark metadata so results identify the selected compact IP layout and whether the non-selected layout was benchmarked and rejected.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `exrabitq-storage-layout`: add optimized official ExData layout metadata and rebuild/version boundaries for direct compact IP.
- `ipexrabitq-compact-layout`: define compact blocked storage for both `2-bit + 1-bit` and `1-bit + 1-bit + 1-bit` 3-bit ExData candidates.
- `exrabitq-stage2-kernel`: add direct compact SIMD IP kernels for the two 3-bit layouts and a selection contract for the faster validated layout.
- `ivf-rabitq-baseline-storage`: persist the selected official ExData layout key in index metadata.
- `ivf-rabitq-baseline-query`: query selected optimized official ExData indexes without query-time full-block decode.
- `benchmark-infra`: record layout comparison results and require a clear winner before replacing official `total_bits=4` results.

## Impact

- Affected code paths: RaBitQ ExData packing, `cluster.clu` writer/reader, resident preload block views, Stage2 official SIMD kernels, Stage2 classifier integration, benchmark metadata, and COCO100k validation scripts.
- Existing legacy signed-magnitude indexes remain unchanged. Existing v13 official indexes remain readable as fallback/validation inputs but are not expected to be the final performance path.
- Optimized official `ex_bits=3` indexes require rebuild because the compact ExData byte layout changes.
- No changes to raw-vector rerank semantics, `data.dat`, payload store behavior, coarse routing, DiskANN baselines, or parquet support.
