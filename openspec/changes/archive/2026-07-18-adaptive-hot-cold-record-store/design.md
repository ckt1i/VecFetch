## Context

RecordGate combined layout currently stores each record as:

```text
[raw_vector][payload]
```

and `cluster.clu` stores an `AddressEntry{offset,size}` into `data.dat`.  This
is simple and works well for small payloads, but on large-payload datasets the
exact rerank path repeatedly reads small raw vectors from a large file dominated
by payload bytes.

Phase 1 of this change implemented a benchmark-facing sidecar layout:

```text
hotvec.dat
payload.cold.dat
hotcold_map.bin
```

It improved over combined-store on VoxCeleb2 and MSMARCO, but retained a global
`combined_offset -> row_id/payload_offset/payload_bytes` map.  On MSMARCO this
map is large and every reranked vector performs a random `unordered_map`
lookup.  The next phase should remove this indirection.

## Goals / Non-Goals

**Goals:**

- Replace the sidecar map with a direct-address inline hot-record format.
- Support two payload placements:
  - inline small payload in the hot record.
  - cold large payload in `payload.cold.dat`, referenced by descriptor.
- Rewrite record-address metadata so query can start from `AddressEntry.offset`
  and directly read the hot record without `hotcold_map.bin`.
- Reuse existing IVF/RaBitQ quantization artifacts where possible; do not
  retrain centroids or rebuild quantized codes.
- Preserve combined-store, no-combine separate-store, and Phase 1 sidecar
  hot/cold benchmark modes.
- Evaluate VoxCeleb2 and MSMARCO with the same query parameters used in the
  Phase 1 comparison.

**Non-Goals:**

- Do not remove Phase 1 sidecar support.
- Do not add payload compression, media decoding, or semantic payload parsing.
- Do not preload large payload bodies into memory.
- Do not require all datasets to use inline payloads; large-payload datasets
  should primarily use cold pointers.

## Decisions

### Decision 1: Use an extensible payload kind instead of a strict boolean

The user-level model is boolean: a hot record either contains raw payload bytes
or contains a pointer to cold payload bytes.  The on-disk format should still use
a one-byte enum so future variants can be added without another format break:

```text
0 = inline_payload
1 = cold_pointer
2 = prefix_cold_pointer  (reserved; not implemented in this phase)
```

Rationale: the current phase intentionally merges the SafeIn payload-read
threshold with the storage placement threshold.  SafeIn can only obtain payload
bytes that are already inline in the hot record.  It does not issue a separate
cold payload prefix read for `cold_pointer` records.  `prefix_cold_pointer`
remains reserved for a future experiment if VoxCeleb2 needs partial inline
prefixes.

### Decision 2: Store a fixed descriptor immediately after the raw vector

Each hot record should begin with:

```text
[raw_vector bytes][HotPayloadDescriptor][inline payload bytes, optional]
```

A concrete descriptor can be:

```cpp
struct HotPayloadDescriptor {
    uint8_t payload_storage_type;
    uint8_t header_size;
    uint16_t flags;
    uint32_t inline_bytes;
    uint64_t payload_offset;
    uint64_t payload_bytes;
};
```

For `inline_payload`, `inline_bytes == payload_bytes` and
`payload_offset == 0`.  For `cold_pointer`, `inline_bytes == 0` and
`payload_offset/payload_bytes` locate bytes in `payload.cold.dat`.
`prefix_cold_pointer` MUST be rejected until explicitly implemented.

Rationale: reading `vec_bytes + sizeof(HotPayloadDescriptor)` for exact rerank
adds only a small fixed overhead and lets the query path cache payload metadata
without a map lookup.  When the descriptor says `inline_payload`, SafeIn can
read the inline payload bytes from the hot record in the same read plan.  When
the descriptor says `cold_pointer`, SafeIn stops at raw vector + descriptor and
final top-k materialization performs the cold payload read only if needed.

### Decision 3: Remap cluster record addresses rather than keep original offsets

To eliminate `hotcold_map.bin`, the derived index must make
`AddressEntry.offset` point directly to the new hot record offset.  A packed hot
record plane is preferred:

```text
derived index:
  cluster.clu        copied quantized data + rewritten record addresses
  data.dat           packed hot records
  payload.cold.dat   large payload bytes
  manifest.json      layout metadata and thresholds
```

The derived hot record file is named `data.dat`.  This keeps the query runner on
the existing data-file open path while the manifest and explicit
`record_layout=inline_hot_record_store` JSON field disambiguate the physical
layout from the legacy combined `[raw_vector][payload]` format.

Alternative A: keep original combined offsets and write a sparse hot file at the
same offsets.  This avoids address rewriting but preserves a large logical
address space and risks fragmentation.

Alternative B: keep the Phase 1 `hotcold_map.bin`.  This preserves index reuse
but retains the exact overhead this phase is meant to remove.

### Decision 4: Query should read vector and descriptor together

For exact rerank under `inline_hot_record_store`, the scheduler should read:

```text
vec_bytes + descriptor_bytes
```

from the hot record.  The reranker consumes only the first `vec_bytes`, while
the scheduler or rerank consumer caches the descriptor by record address for
SafeIn/final payload materialization.

Rationale: this avoids one extra small metadata read per final result and keeps
payload decisions available without an external map.

### Decision 5: Inline payload threshold is the inline-mode SafeIn payload limit

The materializer should accept a threshold such as:

```text
--inline-payload-threshold BYTES
```

Records with `payload_bytes <= threshold` use inline payload.  Larger records
use cold pointer.  For inline hot-record mode there is no separate
`safein_prefetch_threshold`: the storage threshold is the maximum payload size
SafeIn can acquire before final top-k.  Legacy combined and Phase 1 sidecar
layouts may continue to expose `safein_threshold_bytes` for compatibility.

The manifest must include:

```text
layout = inline_hot_record_store
descriptor_bytes
inline_payload_threshold
effective_safein_inline_threshold
record_count
inline_record_count
cold_record_count
hot_record_bytes
cold_payload_bytes
address_map_bytes = 0
```

Rationale: the experiment needs to explain whether speedups come from removing
the map, reducing large-file raw-vector reads, or changing payload placement.
Keeping one threshold in inline mode prevents the system from silently
reintroducing large cold payload reads during SafeIn.

### Decision 6: Evaluation should compare four layouts

The Phase 2 report should include:

```text
combined
existing no-combine separate-store
Phase 1 sidecar hot/cold
Phase 2 inline hot-record
```

Primary comparison: same recall, avg ms, and QPS.  Secondary comparison:
`avg_probe_submit_ms`, `avg_fetch_missing_ms`, `avg_search_unaccounted_ms`,
read requests, read bytes, RSS, and map/descriptor memory.

Expected result:

- MSMARCO should improve more than VoxCeleb2 because it reranks many more
  candidates per query and currently pays many more map lookups.
- VoxCeleb2 should preserve or slightly improve the current positive result
  against separate-store; the gain from removing the map is smaller because the
  average number of reranked vectors is much lower.

## Risks / Trade-offs

- [Risk] Rewriting address metadata is more invasive than the Phase 1 sidecar.
  -> Mitigation: create a derived index directory and keep original indexes
  untouched.

- [Risk] Existing `AddressEntry.size` semantics may not match full payload size
  after large payloads move cold.
  -> Mitigation: define `AddressEntry.size` as hot-record size in the derived
  index and store full payload length in `HotPayloadDescriptor`.

- [Risk] Always reading descriptor bytes can slightly slow pure vector rerank.
  -> Mitigation: descriptor is small and replaces a much more expensive random
  map lookup; include vector-only timing in the report.

- [Risk] Inline payload threshold can hurt large-payload datasets if too high.
  -> Mitigation: start with threshold 0 or a small value for VoxCeleb2/MSMARCO.
  Do not use the old 256KB SafeIn threshold as the default storage threshold;
  only sweep larger values as an explicit storage/SafeIn ablation.

- [Risk] A direct-address hot record may look similar to no-combine
  separate-store.
  -> Mitigation: document the key distinction: RecordGate cluster addresses
  directly index the record storage, while baseline separate-store keeps vector
  and payload planes behind a map/backend abstraction.

## Migration Plan

1. Keep Phase 1 files and tests as baseline.
2. Add the descriptor and manifest definitions.
3. Implement a materializer that writes a derived inline hot-record index under:

   ```text
   /home/zcq/VDB/test/data/<dataset>/indexes/recordgate/inline_hot_record_store_<variant>/
   ```

4. Update query initialization to detect or select the inline hot-record layout.
5. Add tests for inline payload, cold pointer, descriptor mismatch, and final
   suffix reconstruction.
6. Run smoke tests, then three-repeat VoxCeleb2/MSMARCO comparisons.

Rollback: run benchmarks without the inline hot-record index or select
`combined`, `separate-store`, or Phase 1 `hotcold_record_store`.

## Resolved Questions and Follow-ups

- If VoxCeleb2 loses too much SafeIn benefit when large payloads are deferred,
  should `prefix_cold_pointer` be implemented as a later storage/SafeIn sweep?
- Address remapping reuses the existing cluster/address serialization helpers
  by creating a derived index and rewriting only the address payloads.
- The inline threshold and SafeIn payload prefetch threshold are intentionally
  the same parameter in inline mode.  `safein_prefetch_threshold` remains only
  as a legacy compatibility field for combined and Phase 1 sidecar layouts.
