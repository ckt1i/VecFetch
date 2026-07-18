## 1. Format and Materialization

- [x] 1.1 Define hot/cold map header and record structs with magic/version checks.
- [x] 1.2 Add a benchmark materializer that emits `hotvec.dat`, `payload.cold.dat`, and `hotcold_map.bin` from an existing combined index.
- [x] 1.3 Add manifest/log output for materialized record count, vector bytes, payload bytes, duplicate references, and wall time.

## 2. Query Integration

- [x] 2.1 Extend benchmark CLI/config to accept a hot/cold store directory and report `record_layout=hotcold_record_store`.
- [x] 2.2 Load hot/cold map files and expose lookup metadata through the existing scheduler store configuration.
- [x] 2.3 Update `OverlapScheduler` read paths so hot/cold layout reads vectors from `hotvec.dat` and payload prefix/suffix from `payload.cold.dat`.
- [x] 2.4 Preserve combined-store and existing no-combine separate-store behavior.

## 3. Verification

- [x] 3.1 Add or update unit/integration tests for hot/cold map validation and prefix/suffix payload completion.
- [x] 3.2 Build relevant targets and run focused tests.
- [x] 3.3 Run a smoke query on one dataset to verify recall and JSON layout reporting.

## 4. Experiments

- [x] 4.1 Materialize hot/cold stores under `/home/zcq/VDB/test/data/voxceleb2_ecapa_150k/indexes/recordgate/` and `/home/zcq/VDB/test/data/msmarco_passage/indexes/recordgate/`.
- [x] 4.2 Run VoxCeleb2 combined, existing separate-store, and hot/cold comparisons with reused indexes.
- [x] 4.3 Run MSMARCO combined, existing separate-store, and hot/cold comparisons with reused indexes.
- [x] 4.4 Write raw JSON/logs, CSV summaries, and a Chinese result report under a dedicated `/home/zcq/VDB/test/recordgate_hotcold_*` directory.

## 5. Review and Documentation

- [x] 5.1 Update `AUTO_REVIEW.md` and `REVIEW_STATE.json` with a self-contained review of the implementation, results, weaknesses, and next steps.
- [x] 5.2 Mark the OpenSpec tasks complete only after implementation evidence exists for each item.

## 6. Phase 2 Inline Hot-Record Format

- [x] 6.1 Define `HotPayloadDescriptor` and payload placement enum for inline, cold pointer, and reserved prefix-cold records; reject prefix-cold until implemented.
- [x] 6.2 Add manifest fields for `inline_hot_record_store`, descriptor bytes, inline threshold, effective SafeIn inline threshold, inline/cold record counts, hot bytes, cold bytes, and `address_map_bytes=0`.
- [x] 6.3 Decide and document whether the derived hot record file is named `data.dat` or `data.hot.dat`, then keep query CLI and manifests consistent.

## 7. Phase 2 Materialization and Address Remap

- [x] 7.1 Implement a materializer mode or new benchmark target that writes packed inline hot records and `payload.cold.dat` from an existing combined index.
- [x] 7.2 Rewrite derived index record-address metadata so each `AddressEntry.offset` points directly to the packed hot record.
- [x] 7.3 Preserve IVF/RaBitQ quantized cluster data, centroids, epsilon caches, and query-visible index parameters while remapping only record storage/address metadata.
- [x] 7.4 Materialize derived inline hot-record stores under `/home/zcq/VDB/test/data/voxceleb2_ecapa_150k/indexes/recordgate/` and `/home/zcq/VDB/test/data/msmarco_passage/indexes/recordgate/`.

## 8. Phase 2 Query Integration

- [x] 8.1 Add CLI/config support for selecting `record_layout=inline_hot_record_store`.
- [x] 8.2 Update query initialization so inline mode opens the hot record file and optional `payload.cold.dat` without loading `hotcold_map.bin` or `address_map.bin`.
- [x] 8.3 Update exact rerank reads to fetch `raw_vector + HotPayloadDescriptor` from the hot record and feed only raw vector bytes to rerank.
- [x] 8.4 Update SafeIn and final materialization so inline payloads are read from the hot record, while cold payloads are deferred until final top-k and then read from `payload.cold.dat`.
- [x] 8.5 Add JSON diagnostics showing inline mode uses zero sidecar map bytes/records and reports descriptor memory overhead separately.

## 9. Phase 2 Verification

- [x] 9.1 Add unit tests for descriptor serialization, unknown descriptor rejection, inline payload reconstruction, and cold payload final reconstruction without SafeIn cold-prefix reads.
- [x] 9.2 Add integration tests proving inline mode does not require `hotcold_map.bin` and preserves recall/result ordering against the equivalent combined index.
- [x] 9.3 Build relevant targets and run focused tests before launching full experiments.
- [x] 9.4 Run smoke queries on VoxCeleb2 and MSMARCO to verify recall, JSON layout reporting, and no sidecar map loading.

## 10. Phase 2 Experiments and Review

- [x] 10.1 Run VoxCeleb2 comparisons for combined, existing separate-store, Phase 1 sidecar hot/cold, and Phase 2 inline hot-record using matched query parameters.
- [x] 10.2 Run MSMARCO comparisons for combined, existing separate-store, Phase 1 sidecar hot/cold, and Phase 2 inline hot-record using matched query parameters.
- [x] 10.3 Store raw JSON/logs, CSV summaries, and a Chinese result report under `/home/zcq/VDB/test/recordgate_inline_hot_record_store_*`.
- [x] 10.4 Analyze whether inline hot-record removes enough map/row-id overhead to beat the storage-format ablation baselines on VoxCeleb2 and MSMARCO.
- [x] 10.5 Update `AUTO_REVIEW.md` and `REVIEW_STATE.json` with implementation evidence, external/self review notes, weaknesses, and next steps.
