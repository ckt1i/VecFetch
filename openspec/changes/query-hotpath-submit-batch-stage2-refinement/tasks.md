## 1. Experiment Contract And Baseline Guardrails

- [x] 1.1 对齐 `bench_e2e` 与 `run_hotpath_experiments.py` 的两条固定 operating point：`nprobe=64` 无 CRC early-stop 与 `nprobe=256 + crc-alpha=0.02` 的 CRC early-stop
- [x] 1.2 补齐或整理 benchmark 输出字段，明确 `probe_submit` flush 次数、flush 规模和 Stage2 低开销/细粒度归因口径
- [x] 1.3 用当前两条基线重新跑受控实验，确认固定 probe 约 `0.5ms` 与 CRC early-stop 约 `1.0ms` 的验证起点与已记录结果一致

## 2. Fixed-Parameter Adaptive Submit Batching

- [x] 2.1 在 resident hot path 中实现三层 flush 决策骨架，明确 `hard flush / soft flush / tail flush` 的判定入口
- [x] 2.2 接入固定参数版 submit 策略，包括目标 batch、interval limit 和 tail 最小阈值
- [x] 2.3 为固定 `nprobe` 与 CRC early-stop 两类 probe 模式补齐 stop-sensitive flush 约束，包括 `stop-safe flush` 与 break 前 `flush + drain`
- [x] 2.4 保持 `SafeIn` / `Uncertain`、dedup、rerank 和最终排序语义不变，并为 resident thin path 保留可回退行为
- [x] 2.5 输出与三层 flush、stop-sensitive flush 对应的提交统计字段，确保收益能与旧静态 `submit-batch` 行为直接对比

## 3. Stage2 Collect / Scatter Refinement

- [x] 3.1 将 Stage2 collect 从逐 survivor 线性拼装改为 block-first 组织，并保持 padding lane 排除语义
- [x] 3.2 在当前 Stage2 路径上实现 scatter 的 batch numeric classify 骨架，先生成 classify mask 再做 surviving lane 压缩写回
- [x] 3.3 为 Stage2 collect/scatter 保留低风险 fallback，并确保不引入新索引格式或 resident layout 前置条件
- [x] 3.4 在细粒度归因模式下验证 Stage2 collect、kernel 和 scatter 字段仍然可比较

## 4. Validation And Decision Gate

- [x] 4.1 对固定参数版 `submit-batch` 路径执行多次重复实验，分别覆盖 `nprobe=64` 固定 probe 基线与 `nprobe=256 + crc-alpha=0.02` 的 CRC early-stop 基线，确认收益是否超过噪声区间且 recall 不变
- [x] 4.2 对 Stage2 collect/scatter 精修执行 query-only 与 full E2E 双口径验证，确认 funnel 语义与结果口径不变
- [x] 4.3 整理结果并判断是否开启在线观测版 submit 调度；若收益不足，明确记录暂停条件

## 5. Optional Online-Observation Submit Tuning

- [x] 5.1 在固定参数版验证有效后，引入 query-local 轻量在线统计，例如 `ema_req_per_cluster`
- [x] 5.2 基于在线统计实现动态 target batch / interval 的 gated 调度，不新增新的 flush 语义类别
- [x] 5.3 验证在线观测版可关闭且关闭后可无缝回退到固定参数版三层 flush
