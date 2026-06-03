## 1. Runtime Configuration Cleanup

- [x] 1.1 将 `DynamicSafeInMode` 精简为只包含 `Static` 和 `Frontier`。
- [x] 1.2 从 `SearchConfig` 删除 `dynamic_safein_scale`、`dynamic_safein_scale_cap_to_static`、`dynamic_safein_payload_only` 和 Dynamic SafeIn gap tolerance 字段。
- [x] 1.3 从 `SearchStats` 删除 Dynamic SafeIn gap sample/ready/final stats，同时保留 frontier、threshold、deferred、prefetch/read accounting stats。
- [x] 1.4 更新所有 `DynamicSafeInModeName` 和 parsing helpers，使其只暴露 `static`、`off` 和 `frontier`。

## 2. OverlapScheduler Simplification

- [x] 2.1 精简 readiness 逻辑，只保留 surviving frontier path，并删除 cap、delay、stable、scale、blend、payload-only、gap-gate 模式分支。
- [x] 2.2 在可行范围内，将 accepted dynamic path 的内部命名从 blend terminology 改为 frontier terminology。
- [x] 2.3 将 `SafeInThresholdForProbe()` 的 frontier 行为改为固定 `T_q = F_lower`，不再有 lambda interpolation 或 cap-to-static 行为。
- [x] 2.4 保留 deferred SafeIn candidate buffering、forced final flush 和 ready-time reclassification 语义。
- [x] 2.5 保持 static SafeIn 行为和 Dynamic SafeOut frontier 行为不变。

## 3. Benchmark CLI and Output Cleanup

- [x] 3.1 更新 `bench_vector_search`，使 `--dynamic-safein` 只接受 `static|off|frontier`。
- [x] 3.2 从 `bench_vector_search` 删除已废弃 Dynamic SafeIn CLI flags，包括 scale、scale-cap-static、payload-only 和 gap tolerance options。
- [x] 3.3 更新 `bench_e2e`，使 `--dynamic-safein` 只接受 `static|off|frontier`。
- [x] 3.4 从 `bench_e2e` 删除已废弃 Dynamic SafeIn CLI flags，包括 scale、scale-cap-static、payload-only 和 gap tolerance options。
- [x] 3.5 从 benchmark config JSON、results JSON、per-query CSV 和 logs 删除已废弃字段，同时保留 frontier threshold、deferred candidate、prefetch 和 read-count metrics。
- [x] 3.6 确保 `--dynamic-safein frontier_blend` 和其他已删除 modes 会以清晰 invalid-argument message 被拒绝。

## 4. Tests and Documentation

- [x] 4.1 将已删除 modes 的 Dynamic SafeIn unit tests 替换为 `frontier` threshold `T_q = F_lower` 测试。
- [x] 4.2 新增或保留 frontier 模式下 deferred candidate buffering 和 flush 行为测试。
- [x] 4.3 更新 Dynamic SafeIn 实验脚本，使用不带 scale/lambda 参数的 `--dynamic-safein frontier`，或将 obsolete scripts 标记为 historical。
- [x] 4.4 更新 `safeIn_dk_review_logs/README.md` 和相关 design/report 文档，将 `frontier` 描述为受支持动态模式。

## 5. Verification

- [x] 5.1 构建 `test_overlap_scheduler`、`bench_vector_search` 和 `bench_e2e`。
- [x] 5.2 运行 `ctest --test-dir build -R test_overlap_scheduler --output-on-failure`。
- [x] 5.3 使用 `--dynamic-safein frontier` 运行一个小规模 `bench_vector_search` smoke test。
- [x] 5.4 使用 `--dynamic-safein static` 运行一个小规模 `bench_vector_search` smoke test。
- [x] 5.5 确认已删除 modes 和已删除 flags 会被拒绝。
- [x] 5.6 对修改过的 source、test 和 OpenSpec files 运行 `git diff --check`。
