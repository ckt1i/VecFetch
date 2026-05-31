## Verification Summary

### Implementation

- `simd::AccumulateBlock` public API 未改变。
- AVX512 路径新增：
  - `AccumulateStep16Avx512`
  - `AccumulateBlockAvx512Chunked<32>`
  - `AccumulateBlockAvx512Chunked<64>`
  - `64 -> 32 -> generic` dispatch
- AVX2 路径新增：
  - `AccumulateStep8Avx2`
  - `AccumulateBlockAvx2Chunked<32>`
  - `AccumulateBlockAvx2Chunked<64>`
  - `64 -> 32 -> generic` dispatch
- generic fallback 保留。
- `test_prepare_query` 目标补齐 AVX512/AVX2 编译宏，使测试 reference layout 与 `vdb_simd` 一致。
- `AccumulateBlock` 测试改用 64-byte aligned LUT buffer，满足 kernel contract。

### Correctness

已通过：

- `./build/test_prepare_query`
- `./build/test_rabitq_estimator`
- `./build/test_classify_masks`
- `./build/test_ivf_index`

新增/扩展覆盖维度：

- fallback / non-32 path: `16`
- `n*32` but non-`n*64`: `32`, `96`, `544`, `1056`
- `n*64`: `64`, `512`, `768`, `1024`, `1536`

### Benchmark

正式验证使用真实 GT：

- `--skip-gt 0`
- `--early-stop 0`
- index: `/home/zcq/VDB/test/data/MSMARCO/fht_kac_rotator`
- coarse routing: two-level IVF
- `--two-level-coarse-super-factor 2`
- `--two-level-coarse-budget-factor 8`
- `--fixed-vec-buffer-count 512`

此前记录的 pre-change 对照：

- `avg_query_ms ~= 3.310`
- `recall@10 ~= 0.9310`
- `probe_stage1_ms ~= 1.125`
- `probe_classify_ms ~= 1.283`

本 change inline 后结果：

- output: `/home/zcq/VDB/test/msmarco_accumulate_chunked_template_inline_twolevel_factor2_b8/msmarco_fht_kac_adapter_20260530T232930`
- `recall@10 = 0.9310`
- `avg_query_ms = 3.443`
- `probe_stage1_ms = 1.177`
- `probe_classify_ms = 1.341`
- `probe_submit_ms = 0.888`

未强制内联版本结果：

- `avg_query_ms = 3.498 ~ 3.511`
- `probe_stage1_ms = 1.191 ~ 1.229`

### Perf

perf 文件：

- whole-run: `/tmp/vdb_accumulate_chunked_perf.data`
- delayed/query-skewed: `/tmp/vdb_accumulate_chunked_query_perf.data`

delayed perf 中 `AccumulateBlockAvx512Chunked<64>` 仍约 `0.89%` samples。该 perf 仍包含部分 CRC calibration / OpenMP 工作，因此只能作为热点存在性参考，不作为严格 query-only 占比。

### Conclusion

这轮 change 的正确性成立，且 `n*32 / n*64` chunked dispatch 已落地；但在 MSMARCO 768 维当前主工作点上没有证明端到端性能收益。主要原因是 `AccumulateBlock` 在当前 profile 中已经不是最大的独立热点，chunk 级 unroll 只能减少很少的 loop/control overhead，而 shuffle/add 主体成本仍在。

如果后续继续投入 `AccumulateBlock`，建议不要继续扩大 `<32>/<64>` chunk unroll，而是先用更干净的 query-only perf 分离 Stage1，再考虑二阶段 fixed block-count specialization，例如只对热维度 `768` 或热 block-count 做完全展开，并与当前 chunked path A/B。
