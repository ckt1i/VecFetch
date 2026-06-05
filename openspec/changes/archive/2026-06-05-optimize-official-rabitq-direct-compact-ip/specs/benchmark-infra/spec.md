## ADDED Requirements

### Requirement: Benchmark infrastructure SHALL compare both official 3-bit direct compact layouts
Benchmark infrastructure SHALL provide a repeatable comparison workflow that builds or reuses both optimized official `ex_bits=3` candidate layouts, runs them with identical query controls, and writes a structured comparison summary.

#### Scenario: Both candidate layouts are benchmarked
- **WHEN** the official 3-bit direct compact comparison workflow runs on COCO100k
- **THEN** it MUST run both `2-bit + 1-bit` and `1-bit + 1-bit + 1-bit` layouts
- **AND** both runs MUST use the same query count, GT, `nprobe`, `topk`, and `non_safeout_candidate_budget`

#### Scenario: Comparison summary records required metrics
- **WHEN** the comparison workflow completes
- **THEN** the summary MUST include recall@10, average latency, QPS, p95 latency, peak RSS, resident code bytes, average Stage2 time, Stage2 decode counters, and average rerank vectors for each layout

### Requirement: Benchmark infrastructure SHALL select and record the winning layout
Benchmark infrastructure SHALL apply a deterministic selection rule to the two optimized official 3-bit layouts and record whether the winner is accepted as the selected fast path.

#### Scenario: Winner is selected by latency after correctness
- **WHEN** both candidate layouts pass correctness validation
- **THEN** the workflow MUST select the layout with lower average query latency as the winner
- **AND** if average latency differs by less than 5%, it MUST use peak RSS and implementation simplicity as tie breakers

#### Scenario: Winner is not accepted if speedup is insufficient
- **WHEN** neither candidate layout improves average query latency over the generic v13 official fallback by at least 20%
- **THEN** the workflow MUST mark the fastest candidate as experimental rather than accepted
- **AND** it MUST NOT recommend replacing main official `1+3` results with that candidate

#### Scenario: Accepted winner is ready for main-result rerun
- **WHEN** one candidate passes correctness and acceptance thresholds
- **THEN** the workflow MUST mark it as the selected official 3-bit direct compact layout
- **AND** it MUST provide the index path and result path needed for a main-result rerun
