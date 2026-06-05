## ADDED Requirements

### Requirement: Benchmark infrastructure SHALL remove legacy mode dimensions
Benchmark scripts, test matrices, and result aggregation SHALL remove formal dimensions for HNSW coarse routing, redundant/RAIR assignment, padded Hadamard, and blocked Hadamard.

#### Scenario: Scripts do not generate removed variants
- **WHEN** benchmark scripts generate commands for formal validation
- **THEN** they SHALL NOT emit HNSW routing variants
- **AND** they SHALL NOT emit redundant/RAIR assignment variants
- **AND** they SHALL NOT emit padded or blocked Hadamard variants

#### Scenario: Aggregation does not require removed fields
- **WHEN** benchmark results are aggregated
- **THEN** aggregation SHALL NOT require HNSW fields, RAIR fields, secondary assignment fields, or blocked/padded Hadamard flags
- **AND** it SHALL continue preserving mainline dataset, index, routing, rotation, recall, latency, and SafeIn/SafeOut/Uncertain fields

### Requirement: Legacy-mode cleanup validation SHALL rerun COCO100k and MS MARCO
After implementation, validation SHALL rerun the main COCO100k and MS MARCO benchmark operating points to confirm behavior after deleting legacy search modes.

#### Scenario: COCO100k validation result is recorded
- **WHEN** implementation is complete
- **THEN** validation SHALL run the current COCO100k mainline benchmark or documented test_config
- **AND** the report SHALL include command, index path, query count, `nlist`, `nprobe`, `topk`, `bits`, recall, latency, coarse routing mode, rotation mode, and SafeIn/SafeOut/Uncertain statistics

#### Scenario: MS MARCO validation result is recorded
- **WHEN** implementation is complete
- **THEN** validation SHALL run the current MS MARCO mainline benchmark or documented test_config
- **AND** the report SHALL include command, adapter or dataset path, index path, GT source, query count, `nlist`, `nprobe`, `topk`, `bits`, recall, latency, coarse routing mode, rotation mode, and SafeIn/SafeOut/Uncertain statistics

#### Scenario: Vector-only validation remains available
- **WHEN** `bench_vector_search` is used for COCO100k validation
- **THEN** the command SHALL NOT require `--pad-to-pow2`
- **AND** the output SHALL still include SafeIn/SafeOut/Uncertain and recall fields needed for vector-only comparison
