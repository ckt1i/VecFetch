## ADDED Requirements

### Requirement: Benchmark infrastructure SHALL remove window cluster experiment variants
Benchmark scripts and test matrices SHALL remove formal variants that exist only to compare window cluster reads against full-preload resident query execution.

#### Scenario: Hotpath script no longer emits window-read variants
- **WHEN** hotpath benchmark scripts generate experiment commands
- **THEN** they SHALL NOT emit `--clu-read-mode window`, `--use-resident-clusters 0`, `--prefetch-depth`, `--refill-threshold`, or `--refill-count` variants
- **AND** they SHALL keep resident full-preload as the single cluster-side serving mode

#### Scenario: Result aggregation does not require loading-mode dimension
- **WHEN** benchmark results are aggregated after this change
- **THEN** aggregation SHALL NOT require a window/full-preload loading-mode dimension
- **AND** it SHALL continue preserving dataset, index, `nprobe`, `topk`, SafeIn/SafeOut parameters, preload cost, and query latency fields

### Requirement: Window-read removal validation SHALL retest COCO100k and MS MARCO under the same parameters
After implementation, benchmark validation SHALL rerun COCO100k and MS MARCO using the same operating-point parameters that were used before the cleanup, so the impact of deleting window read control flow is measured without changing algorithmic knobs.

#### Scenario: COCO100k result is rerun and recorded
- **WHEN** implementation is complete
- **THEN** validation SHALL run COCO100k with the current COCO test_config or the documented main anchor parameters
- **AND** the report SHALL include full command, index path, query count, `nlist`, `nprobe`, `topk`, `bits`, epsilon/SafeIn/SafeOut settings, recall, latency, preload metrics, and Stage1/Stage2 SafeIn/SafeOut/Uncertain statistics

#### Scenario: MS MARCO result is rerun and recorded
- **WHEN** implementation is complete
- **THEN** validation SHALL run MS MARCO with the current MS MARCO test_config or the documented main anchor parameters
- **AND** the report SHALL include full command, adapter or dataset path, index path, GT source, query count, `nlist`, `nprobe`, `topk`, `bits`, recall, latency, preload metrics, and Stage1/Stage2 SafeIn/SafeOut/Uncertain statistics

#### Scenario: Same-parameter requirement is explicit
- **WHEN** the validation command differs from the prior test_config
- **THEN** the report SHALL explicitly list the difference and explain whether it affects comparability
- **AND** conclusions SHALL not attribute algorithmic changes to window-read removal unless the operating point is otherwise unchanged
