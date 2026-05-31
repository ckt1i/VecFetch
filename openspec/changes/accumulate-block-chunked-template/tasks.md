## 1. Baseline And Code Boundary

- [x] 1.1 使用 code-review-graph 和源码确认 `simd::AccumulateBlock` 当前 AVX512、AVX2、scalar 路径及所有调用点。
- [x] 1.2 记录当前 two-level IVF baseline 的关键指标：`avg_query_ms`、`probe_stage1_ms`、`probe_classify_ms`、`recall@10` 和 perf 中 `AccumulateBlock` 采样占比。
- [x] 1.3 确认本 change 不需要修改 `AccumulateBlock` public signature、packed code layout 或 packed LUT layout。

## 2. AVX512 Chunked AccumulateBlock

- [x] 2.1 从现有 AVX512 loop body 抽取 `AccumulateStep16Avx512` helper，保持 load、shuffle、accumulate 和 pointer increment 与原逻辑一致。
- [x] 2.2 新增 `AccumulateBlockAvx512Chunked<32>`，每个 chunk 固定执行 2 次 `Step16`。
- [x] 2.3 新增 `AccumulateBlockAvx512Chunked<64>`，每个 chunk 固定执行 4 次 `Step16`。
- [x] 2.4 复用或抽取现有 AVX512 accumulator reduction / combine 逻辑，确保 `result[32]` 输出不变。
- [x] 2.5 在 AVX512 `AccumulateBlock` dispatch 中优先匹配 `dim % 64 == 0`，其次匹配 `dim % 32 == 0`，其他维度回退 generic。

## 3. AVX2 Chunked AccumulateBlock

- [x] 3.1 从现有 AVX2 loop body 抽取 `AccumulateStep8Avx2` helper，保持 load、shuffle、accumulate 和 pointer increment 与原逻辑一致。
- [x] 3.2 新增 `AccumulateBlockAvx2Chunked<32>`，每个 chunk 固定执行 4 次 `Step8`。
- [x] 3.3 新增 `AccumulateBlockAvx2Chunked<64>`，每个 chunk 固定执行 8 次 `Step8`。
- [x] 3.4 复用或抽取现有 AVX2 accumulator reduction / combine 逻辑，确保 `result[32]` 输出不变。
- [x] 3.5 在 AVX2 `AccumulateBlock` dispatch 中使用与 AVX512 一致的 `64 -> 32 -> generic` 分派策略。

## 4. Correctness Tests

- [x] 4.1 为 `AccumulateBlock` 增加 reference accumulation 测试工具，能够生成随机 packed codes / LUT 并比较 `result[32]`。
- [x] 4.2 覆盖 `n*64` 维度测试，例如 `64`、`512`、`768`、`1024`、`1536`。
- [x] 4.3 覆盖 `n*32` 但非 `n*64` 维度测试，例如 `32`、`96`、`544`、`1056`。
- [x] 4.4 覆盖非 `32` 倍数但合法维度 fallback 测试，确认结果与参考一致。
- [x] 4.5 保留现有 `PrepareQuery` / FastScan distance 等价测试，确认 Stage1 estimate 语义不变。

## 5. Build And Regression

- [x] 5.1 构建受影响目标：`cmake --build build --target test_prepare_query bench_e2e -j$(nproc)` 或实际仓库中的对应测试目标。
- [x] 5.2 运行 SIMD/FastScan 相关单元测试。
- [x] 5.3 运行现有 `test_ivf_index` 或 query pipeline smoke 测试，确认没有影响 Stage1 调用链。

## 6. Benchmark And Perf Validation

- [x] 6.1 使用真实 GT、`--skip-gt 0`、`--early-stop 0` 和当前分层 IVF baseline 命令重跑 MSMARCO `fht_kac_rotator` benchmark。
- [x] 6.2 对比 `avg_query_ms`、`probe_stage1_ms`、`probe_classify_ms`、`recall@10`，确认 recall/top-k 不回退。
- [x] 6.3 运行 focused perf，确认 `simd::AccumulateBlock` 或其子路径采样占比下降，且没有出现新的异常热点。
- [x] 6.4 如果 chunked path 收益不足但 `AccumulateBlock` 仍是明显热点，记录是否需要二阶段固定 block-count specialization，不在本 change 内实现。

## 7. Documentation And Reporting

- [x] 7.1 在 change 结果中记录最终采用的 dispatch 策略和覆盖维度。
- [x] 7.2 报告性能结果时必须注明是否使用真实 recall，并列出 baseline 与 optimized 的完整命令差异。
- [x] 7.3 若性能收益低于预期，明确区分是 `AccumulateBlock` 本身已不再是主热点，还是 chunk 模板化无法有效降低 shuffle/add 主体成本。
