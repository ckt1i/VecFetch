## Why

The first hot/cold sidecar implementation shows that separating large payload
bytes from the raw-vector rerank path improves VoxCeleb2 and MSMARCO over the
combined `data.dat` layout.  However, that implementation still uses
`hotcold_map.bin` to translate the original combined record offset into a hot
vector row and cold payload slice, so every rerank candidate pays a global map
lookup that is especially visible on large datasets such as MSMARCO.

This change is extended with a second phase: replace the sidecar map with an
inline hot-record format where cluster addresses directly point to records of
the form `raw_vector + payload descriptor + optional inline payload`.  Large
payload bodies move to `payload.cold.dat`; small payloads can remain inline.

## What Changes

- Keep the completed Phase 1 sidecar format as an experimental baseline:
  - `hotvec.dat`: dense raw-vector plane.
  - `payload.cold.dat`: cold payload bytes.
  - `hotcold_map.bin`: original combined offset to hot/cold location map.
- Add a Phase 2 inline hot-record format:
  - A hot record stores `[raw_vector][payload_storage_type][payload_offset][payload_bytes]`
    for cold payloads.
  - A hot record stores `[raw_vector][payload_storage_type][raw_payload]`
    for inline small payloads.
  - The implementation SHOULD use an extensible one-byte payload kind rather
    than a strict boolean, reserving values for `inline`, `cold_pointer`, and
    possible `prefix_cold` records.
  - New cluster addresses MUST point directly to the hot record offset, so
    query no longer needs `hotcold_map.bin` or an `unordered_map` lookup.
  - In inline mode, the storage threshold is also the SafeIn payload-read
    threshold: SafeIn may obtain inline payload bytes already stored in the hot
    record, but it MUST NOT issue cold payload prefix reads for records whose
    payload is stored separately.
- Add a materialization/remap path that builds the inline hot-record store from
  an existing RecordGate index without retraining IVF/RaBitQ:
  - Copy or reuse quantized cluster data.
  - Rewrite only record-address metadata so `AddressEntry.offset` targets the
    new hot record plane.
  - Emit `payload.cold.dat` for large payload bodies and a manifest describing
    layout thresholds and byte counts.
- Extend query/benchmark paths to consume the inline hot-record store directly:
  - Read `raw_vector + descriptor` from the hot record for exact rerank.
  - Read inline payload bytes from the hot record when present.
  - Defer large cold payload reads until final top-k materialization using the
    descriptor.
  - Report `record_layout=inline_hot_record_store` and diagnostic evidence that
    no sidecar map is loaded.
- Preserve existing combined-store, no-combine separate-store, and Phase 1
  sidecar hot/cold behavior.
- Re-run VoxCeleb2 and MSMARCO comparisons with reused quantization/index
  artifacts where possible, storing all new stores and results under
  `/home/zcq/VDB/test`.

## Capabilities

### New Capabilities

- `adaptive-hot-cold-record-store`: Defines both the completed sidecar hot/cold
  store and the new inline hot-record store, including materialization,
  address-remapping, query semantics, and large-payload experiment evidence.

### Modified Capabilities

- `payload-pipeline`: Adds descriptor-driven inline/cold payload fetch semantics
  while preserving existing combined-store and sidecar hot/cold behavior.
- `query-pipeline`: Adds a direct-address inline hot-record mode that avoids
  sidecar map lookup while preserving recall, top-k, SafeIn, and final payload
  fetch semantics.

## Impact

- Affected code:
  - materialization utilities for record-plane rewrite/remap.
  - storage/address metadata handling for direct hot-record offsets.
  - benchmark CLI parsing and JSON/CSV result reporting.
  - `OverlapScheduler` raw-vector and payload read paths.
  - query pipeline tests covering inline, cold-pointer, and suffix-fetch cases.
- New experiment artifacts:
  - `/home/zcq/VDB/test/recordgate_inline_hot_record_store_<date>/`
  - `/home/zcq/VDB/test/data/{voxceleb2_ecapa_150k,msmarco_passage}/indexes/recordgate/inline_hot_record_store_*`
- Compatibility:
  - Existing combined indexes remain readable.
  - Existing no-combine separate-store and Phase 1 sidecar hot/cold stores
    remain benchmark-selectable.
  - Phase 2 may create a derived index directory with rewritten record
    addresses, but it MUST NOT retrain IVF centroids or rebuild RabitQ codes.
