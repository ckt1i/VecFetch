## MODIFIED Requirements

### Requirement: The raw vector plane SHALL support fixed-cost row-id addressing
The baseline SHALL support exact rerank by loading raw vectors directly from disk using row id addressing. For padded-Hadamard indexes, the raw vector plane MUST store vectors in `logical_dim`, while the compressed cluster code plane MAY continue to use `effective_dim`. Index construction SHALL continue to reuse existing centroids; this change does not retrain or replace clustering.

#### Scenario: Raw vectors use logical-dimension row-major addressing under padded-Hadamard
- **WHEN** an index is built with padded-Hadamard enabled for a non-power-of-two input such as `logical_dim=768` and `effective_dim=1024`
- **THEN** the raw vector plane SHALL store only `768` float values per record
- **AND** row-id addressing SHALL remain fixed-cost and independent of cluster layout

#### Scenario: Compressed code and raw vector planes use different dimensions
- **WHEN** a padded-Hadamard index is reopened
- **THEN** the compressed cluster/code plane MAY continue to operate in `effective_dim`
- **AND** the raw vector plane SHALL remain readable in `logical_dim`
- **AND** the storage contract SHALL NOT require padded raw vectors to exist on disk
