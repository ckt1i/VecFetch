## ADDED Requirements

### Requirement: SafeIn optional payload I/O SHALL not bypass mandatory backlog through early-submit
The payload pipeline SHALL schedule optional SafeIn payload I/O through the normal optional-I/O admission, capacity, submit, and drain policy. It MUST NOT allow a first batch of optional requests to bypass a mandatory backlog through `safein_optional_io_early_submit_max_requests` or an equivalent deleted early-submit branch.

#### Scenario: Mandatory backlog blocks optional payload submission
- **WHEN** mandatory vector I/O is above the scheduler's normal optional-I/O admission threshold
- **THEN** newly queued optional SafeIn payload I/O MUST remain pending under the normal policy
- **AND** it MUST NOT be submitted by a bounded early-submit exception

#### Scenario: Optional payload I/O proceeds after normal admission becomes available
- **WHEN** mandatory pressure clears and the ordinary optional-I/O admission conditions are satisfied
- **THEN** eligible SafeIn payload I/O MUST be allowed to submit through the normal optional-I/O path
- **AND** removal of early-submit MUST NOT disable optional payload prefetch as a whole

### Requirement: Removing optional early-submit SHALL preserve final payload completeness
The payload pipeline MUST preserve final top-k payload materialization regardless of whether an optional SafeIn payload was prefetched. Any payload not available from span reuse or normal optional prefetch MUST be fetched by the final materialization path.

#### Scenario: Deferred optional payload is fetched at finalization
- **WHEN** a final top-k record's payload was not submitted through normal optional prefetch
- **THEN** final materialization MUST fetch the missing payload
- **AND** the returned record contents MUST match the reference no-cap execution

