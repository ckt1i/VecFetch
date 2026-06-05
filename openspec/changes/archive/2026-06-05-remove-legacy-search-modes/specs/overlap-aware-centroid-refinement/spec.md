## REMOVED Requirements

### Requirement: centroid 必须支持在 overlap membership 下做 refinement
**Reason**: Overlap membership is removed from the formal build path, so overlap-aware centroid refinement is no longer applicable.
**Migration**: Build centroids for single-assignment IVF only.

### Requirement: Overlap-aware refinement 必须保留可对照的 partition 输出
**Reason**: Refined and non-refined overlap variants are no longer benchmark dimensions.
**Migration**: Coarse partition diagnostics SHALL focus on single-assignment outputs.
