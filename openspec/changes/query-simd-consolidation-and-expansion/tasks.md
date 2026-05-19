## 1. SIMD Directory Consolidation

- [x] 1.1 盘点当前散落在 `src/index`、`src/query` 等业务模块中的 SIMD 逻辑，并明确迁移目标文件到 `src/simd` 与 `include/vdb/simd`
- [x] 1.2 为迁移后的 Stage2 classify helper 建立独立 `simd` 接口，并从 `cluster_prober.cpp` 中移除内嵌 SIMD 实现细节
- [x] 1.3 为迁移后的 Stage1 safe-in helper 并入 `simd/fastscan.*` 或等价模块，并从业务层移除内嵌 ISA 细节
- [x] 1.4 清理业务层残留的 ISA 分支，确保业务代码只保留 feature gate、scratch 准备和 `simd::*` 调用

Priority: Highest. `1.x` 完成前，不进入新增 runtime path 的 Phase 2。

## 2. Query SIMD Runtime Phase 1

- [x] 2.1 为地址批解码新增 `simd` 目录下的统一接口、保守 batch 实现与 scalar fallback，并接入 `DecodeAddressBatch()`
- [x] 2.2 为 rerank 新增 `simd/rerank_distance.*` 接口与第一版 batched exact-distance kernel，并在 `RerankConsumer` 中接入可切换调用路径
- [x] 2.3 为 coarse top-n 新增 `simd/coarse_select.*` 接口与第一版 small-topn specialized wrapper，并在 `FindNearestClusters()` 中保留 fallback
- [x] 2.4 为每个新增 query SIMD 能力补齐显式开关，确保 capability 可独立启用和关闭

Priority order inside `2.x`:
- `2.1` and `2.2` before `2.3`
- `2.3` only after `2.1` / `2.2` have stable correctness and benchmark wiring

## 3. Fallback And Phase 1 Validation

- [x] 3.1 为 address decode、rerank batched distance、coarse small-topn 和 Stage2 classify 统一 fallback 约束
- [x] 3.2 确认所有新的 `simd` 接口都由业务层通过稳定 helper 调用，而不是在调用点展开 AVX 逻辑
- [x] 3.3 检查新增 SIMD 能力在关闭时能够无缝回退到当前参考路径，且不改变结果消费者契约
- [x] 3.4 完成 Phase 1 的 query-only 与 full E2E correctness 对照，确认进入 Phase 2 的前提成立

Gate to Phase 2:
- `3.4` 完成前，不开始 `4.x`

## 4. Query SIMD Runtime Phase 2

- [ ] 4.1 为 address decode 在不改变接口的前提下追加更强的 SIMD specialization，并保留 Phase 1 fallback
- [ ] 4.2 为 rerank batched distance 在不改变接口的前提下追加更强的 SIMD specialization，并保留 Phase 1 fallback
- [x] 4.3 为 coarse small-topn 在不改变接口的前提下追加更强的 specialized/vectorized path，并保留 Phase 1 fallback
- [x] 4.4 仅在对应能力的 Phase 1 correctness 与 benchmark 稳定后，才启用 Phase 2 的 benchmark 比较
  - 当前仅 coarse_select 进入 Phase 2 benchmark comparison；address decode / rerank batched distance 保持 Phase 1
  - 低负载重测已完成，Phase 2 仍不建议默认开启

Priority order inside `4.x`:
- `4.3` 独立推进，不阻塞 `4.1` / `4.2`
- `4.1` / `4.2` 暂缓，保留 Phase 1 作为默认实现

## 5. Benchmark And Validation

- [x] 5.1 扩展 benchmark 输出，使 SIMD capability 开关状态与 Phase 1/Phase 2 运行模式在结果中显式可见
- [x] 5.2 对 Stage2 current/fallback 路径执行 `query_only` 与 `full_e2e` 双口径细粒度验证，确认 `collect/kernel/scatter` 字段可比
- [x] 5.3 对 coarse small-topn 执行 Phase 1 / fallback / Phase 2 对照实验，确认 recall 不变且收益超过噪声区间；address decode 与 rerank batched distance 继续停留在 Phase 1
- [x] 5.3 对 coarse small-topn 执行 Phase 1 / fallback / Phase 2 对照实验，确认 recall 不变且收益超过噪声区间；address decode 与 rerank batched distance 继续停留在 Phase 1
  - 已在低负载条件下复测，Phase 2 仍维持默认关闭
- [ ] 5.4 对在线 submit tuning 保持默认关闭，并通过 on/off 对照确认关闭后无缝回退、开启后不破坏 CRC early-stop 语义

## 6. Decision And Cleanup

- [ ] 6.1 汇总各 SIMD capability 在 Phase 1 与 Phase 2 下的收益、风险和默认开关建议，明确哪些能力默认开启
- [ ] 6.2 若某项 query SIMD 在 Phase 2 收益不足，记录暂停结论并保留 Phase 1 fallback 与 benchmark 路径
- [ ] 6.3 清理遗留的局部 SIMD helper 和重复实现，确保最终目录结构以 `simd` 目录为唯一实现源
