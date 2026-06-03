## ADDED Requirements

### Requirement: Query pipeline SHALL keep resident vector-read submit-prep batch-first
resident 查询管线 SHALL keep vector-read submit-prep as a batch-first optimization boundary. The pipeline MUST allow candidate batch output to be transformed into compact vector read requests without reverting to per-candidate opaque submit logic. This boundary MUST preserve recall/top-k semantics, submit flush semantics, and async reader in-flight accounting.

#### Scenario: Candidate batch becomes compact read staging
- **WHEN** resident probe produces a candidate batch with vector-only and safein-all candidates
- **THEN** query pipeline MUST allow the batch to be staged as compact read request data
- **AND** the staging MUST remain visible as a separate optimization boundary before io_uring prep

#### Scenario: Submit-prep compaction preserves result semantics
- **WHEN** compact submit-prep is enabled
- **THEN** final top-k results and recall calculation MUST remain equivalent to the reference path
- **AND** benchmark comparison MUST use real ground truth rather than skip-GT-only timing

#### Scenario: In-flight accounting remains unchanged
- **WHEN** compact vector-read submit-prep emits async reads
- **THEN** async reader prepped, submitted, in-flight and completion accounting MUST remain consistent with the reference path
- **AND** final drain MUST not leave query-owned reads pending

### Requirement: Query pipeline SHALL not fuse request generation into direct OnCandidates submit in the first compaction stage
The first vector-read submit compaction stage SHALL keep request generation and actual io_uring prep separated by the existing pending request boundary. The implementation MUST NOT directly submit reads inside `OnCandidates` as part of this change, because submit budget, stop-safe flush, tail flush and final drain semantics must remain independently testable.

#### Scenario: Pending boundary remains present
- **WHEN** `AsyncIOSink::OnCandidates` processes a candidate batch
- **THEN** it MUST produce compact pending request state rather than directly prep all io_uring reads
- **AND** `EmitPendingDataRequests` or its internal vector-only emit path MUST remain responsible for read prep under submit budget

#### Scenario: Stop-sensitive flow remains compatible
- **WHEN** CRC early-stop or stop-sensitive flush logic is active
- **THEN** compact submit-prep MUST remain compatible with pre-stop flush and break-time drain
- **AND** it MUST NOT rely on direct `OnCandidates` submit to preserve correctness
