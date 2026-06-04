## REMOVED Requirements

### Requirement: Secondary assignment 必须支持 recall-aware 的互补选择
**Reason**: All secondary-assignment and overlap policies are removed from the formal build path.
**Migration**: Use single assignment; recall-aware overlap experiments are no longer a supported build mode.

### Requirement: Recall-aware secondary assignment 必须可与现有 overlap mode 比较
**Reason**: The benchmark and diagnostic workflow no longer compares overlap policies after the single-assignment consolidation.
**Migration**: Remove recall-aware, residual-aware, and naive overlap policy dimensions from new diagnostics.
