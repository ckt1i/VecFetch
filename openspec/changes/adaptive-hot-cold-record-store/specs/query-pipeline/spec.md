## ADDED Requirements

### Requirement: Query pipeline supports sidecar hot/cold record layout mode
The query pipeline SHALL keep supporting the Phase 1 sidecar hot/cold record
layout mode that reads raw vectors and payload bytes from separate physical
planes while preserving recall and top-k semantics.

#### Scenario: Sidecar hot/cold query preserves recall
- **WHEN** combined-store and sidecar hot/cold-store queries use the same index,
  query file, ground truth, `topk`, `nprobe`, RabitQ bits, SafeOut, and SafeIn
  settings
- **THEN** `recall_at_k` MUST be identical within deterministic benchmark
  output for the same candidate set
- **AND** final result ordering MUST remain distance-sorted

#### Scenario: Sidecar hot vector reads use fixed-size vector addressing
- **WHEN** a candidate requires exact rerank under sidecar hot/cold layout
- **THEN** the scheduler MUST read exactly `vec_bytes` from `hotvec.dat`
- **AND** the hot vector read offset MUST be derived from the mapped hot vector
  row, not from the cold payload offset

### Requirement: Query pipeline supports inline hot-record layout mode
The query pipeline SHALL support an inline hot-record layout mode whose cluster
addresses directly identify hot records containing raw vectors and payload
descriptors.

#### Scenario: Inline hot-record query preserves recall
- **WHEN** combined-store and inline hot-record queries use equivalent
  quantized cluster data, query file, ground truth, `topk`, `nprobe`, RabitQ
  bits, SafeOut, and SafeIn settings
- **THEN** `recall_at_k` MUST be identical within deterministic benchmark
  output for the same candidate set
- **AND** final result ordering MUST remain distance-sorted

#### Scenario: Inline exact rerank reads vector and descriptor from hot record
- **WHEN** a candidate requires exact rerank under inline hot-record layout
- **THEN** the scheduler MUST read the raw vector bytes from
  `AddressEntry.offset`
- **AND** it MUST read the payload descriptor adjacent to the raw vector
- **AND** it MUST feed only the raw vector bytes into exact rerank
- **AND** for `cold_pointer` descriptors it MUST defer cold payload bytes until
  final top-k materialization

#### Scenario: Inline query avoids sidecar map lookup
- **WHEN** query runs with `record_layout=inline_hot_record_store`
- **THEN** query initialization MUST NOT load `hotcold_map.bin` or
  `address_map.bin`
- **AND** per-query record access MUST NOT perform `combined_offset -> row_id`
  lookup
- **AND** benchmark JSON MUST expose a diagnostic showing sidecar map bytes or
  sidecar record count is zero for the inline layout

#### Scenario: Inline mode is benchmark-selectable
- **WHEN** `bench_e2e` or `bench_online_query` is invoked with the inline
  hot-record store argument or a derived inline index directory
- **THEN** it MUST open the hot record file and optional `payload.cold.dat`
- **AND** it MUST pass descriptor-aware layout metadata to the query scheduler
- **AND** it MUST report `record_layout=inline_hot_record_store`

### Requirement: Hot/cold experiments compare against existing layouts
The query experiment report SHALL compare inline hot-record results against the
current combined-store, existing separate-store, and Phase 1 sidecar hot/cold
baselines using the same query settings.

#### Scenario: VoxCeleb2 and MSMARCO comparison table exists
- **WHEN** the Phase 2 experiment scripts finish
- **THEN** the result summary MUST include combined, separate-store, Phase 1
  sidecar hot/cold, and Phase 2 inline hot-record rows for VoxCeleb2 and
  MSMARCO
- **AND** each row MUST include recall, avg ms, QPS, p95, p99, read requests,
  read bytes, relevant pipeline timing fields, and layout memory overhead

#### Scenario: Inline improvement is analyzed against sidecar overhead
- **WHEN** Phase 2 results are summarized
- **THEN** the report MUST include the delta between sidecar hot/cold and inline
  hot-record layouts
- **AND** it MUST discuss whether any speedup is consistent with removing
  `hotcold_map.bin` and row-id lookup overhead
