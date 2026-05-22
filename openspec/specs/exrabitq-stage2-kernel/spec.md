# Spec: ExRaBitQ Stage 2 Kernel

## Overview

Two-stage optimization path for IPExRaBitQ: Stage 1 optimizes the kernel without storage format changes, Stage 2 (if needed) considers storage layout changes.

## Requirements

### Requirement: Two-Stage ExRaBitQ Optimization Path
The system SHALL treat `IPExRaBitQ` optimization as a two-stage capability. Stage 1 SHALL optimize the Stage2 kernel without changing the persisted ex-code storage format. Stage 2 SHALL only be considered after Stage 1 has been evaluated with clean perf and full E2E results.

#### Scenario: Stage 1 remains storage-compatible
- **WHEN** Stage 1 of `IPExRaBitQ` optimization is implemented
- **THEN** the system SHALL continue to consume the existing `code_abs + sign + xipnorm` ex-code layout without requiring index rebuild or format migration

#### Scenario: Stage 2 is gated by profiling
- **WHEN** Stage 1 results are available
- **THEN** Stage 2 SHALL only be entered if clean perf still shows `IPExRaBitQ` as a significant query CPU hotspot

### Requirement: Stage 1 Kernel Must Be Cross-Dimension and AVX-512 First
The Stage 1 `IPExRaBitQ` kernel SHALL target `AVX-512` first and SHALL be organized as a cross-dimension block kernel rather than a single-dimension-specialized implementation.

#### Scenario: Common dimensions use the same block strategy
- **WHEN** the system runs on dimensions such as 96, 128, 256, 384, 512, 768, 1024, or 1536
- **THEN** the Stage 1 kernel SHALL process them through a common block hierarchy rather than a `dim=512`-only fast path

#### Scenario: Tail dimensions remain correct
- **WHEN** the dimension is not an exact multiple of the largest preferred block size
- **THEN** the Stage 1 kernel SHALL fall back through smaller blocks or tails while preserving correctness

### Requirement: Stage 1 Kernel Shall Avoid Signed-Float Code Reconstruction
The Stage 1 `IPExRaBitQ` kernel SHALL avoid the current code-centric path of reconstructing a full signed float code vector before the dot product and SHALL instead consume magnitude and sign as separate inputs.

#### Scenario: Query-centric sign application
- **WHEN** the Stage 1 kernel evaluates a Stage2 candidate
- **THEN** it SHALL apply sign information through a query-centric or equivalent separated-consumption path instead of rebuilding a full signed float code vector

#### Scenario: Bias term remains mathematically equivalent
- **WHEN** the Stage 1 kernel separates magnitude consumption from the `+0.5` bias term
- **THEN** the resulting `ip_raw` SHALL remain mathematically equivalent to the reference `IPExRaBitQ` result

### Requirement: Stage 2 Layout Escalation Order
If Stage 2 layout changes are required after Stage 1, the system SHALL consider `sign bit-pack` before `compact ex-code layout`.

#### Scenario: Sign bit-pack evaluated first
- **WHEN** Stage 1 is insufficient and Stage 2 layout work is approved
- **THEN** the next storage-level optimization step SHALL be `sign bit-pack` before full compact ex-code layout redesign

#### Scenario: Compact layout is treated as a larger migration
- **WHEN** compact ex-code layout is proposed
- **THEN** the design SHALL treat it as a broader storage and migration change rather than as a kernel-only adjustment

## 新增要求

### Requirement: Stage 2 compact kernel 应将 sign handling 和 magnitude consumption 作为可分离的热路径组件来优化
