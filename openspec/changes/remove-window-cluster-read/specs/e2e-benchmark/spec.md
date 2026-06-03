## MODIFIED Requirements

### Requirement: E2E benchmark output SHALL support `.clu` loading-mode comparison
The E2E benchmark workflow SHALL no longer support formal before/after comparison between sliding-window `.clu` loading and full-preload `.clu` loading. The benchmark SHALL run the main search path under resident full-preload semantics and SHALL output enough metadata to distinguish preload cost from measured query cost.

#### Scenario: Operating point is run under resident full-preload mode
- **WHEN** the benchmark evaluates the main search path
- **THEN** it SHALL run the configured dataset and search parameters under resident full-preload mode
- **AND** it SHALL NOT run or export a `window` loading-mode variant as part of the official result

#### Scenario: Query-speed comparison fields are exported
- **WHEN** a benchmark result is exported
- **THEN** the result SHALL include `recall@10`, average latency, p50/p95/p99 latency where available, and the query-path breakdown fields needed for attribution
- **AND** it SHALL include enough metadata to reconstruct the exact operating point being measured

#### Scenario: Preload cost is recorded alongside query results
- **WHEN** resident full-preload mode is used
- **THEN** the benchmark output SHALL record preload time and preload-related resident memory or byte footprint
- **AND** these fields SHALL be reported alongside the query-speed result rather than omitted

## ADDED Requirements

### Requirement: E2E benchmark CLI SHALL remove window cluster loading knobs
`bench_e2e` SHALL remove or reject formal use of window cluster loading knobs, including `--clu-read-mode window`, `--use-resident-clusters`, `--prefetch-depth`, `--refill-threshold`, and `--refill-count`.

#### Scenario: Removed window options are not accepted as formal controls
- **WHEN** a user invokes `bench_e2e` with a removed window cluster loading option
- **THEN** the benchmark SHALL either reject the option with a clear error or ignore it only through an explicitly documented temporary compatibility path
- **AND** the query behavior SHALL remain resident full-preload

#### Scenario: Config output omits removed knobs
- **WHEN** `bench_e2e` writes config JSON or CSV result metadata
- **THEN** it SHALL NOT report removed prefetch/refill/window knobs as active tuning parameters
- **AND** it SHALL continue reporting preload and resident-memory metrics
