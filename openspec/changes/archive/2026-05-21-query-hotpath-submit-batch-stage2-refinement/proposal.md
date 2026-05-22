## Why

这一轮 profiling 和对照实验已经说明：`submit-batch` 能带来稳定但有限的收益，而 Stage2 的大规模 compute-ready / layout 重构在当前代码上多次尝试后都没有稳定回报。现在需要把优化主线收敛到更低风险、可验证的方向：继续压缩 `probe_submit` 调度骨架，并在不重做 Stage2 布局的前提下，把 `collect/scatter` 这两段明显的标量骨架推进到 batch-first 形态。同时，`submit-batch` 不能再只按固定 `nprobe` 场景理解；它必须与当前两种 probe 生命周期兼容：固定探测 `nprobe` 个 cluster，以及在 `nprobe` 上限下由 CRC 驱动的 early-stop 探测。

## What Changes

- 将现有固定阈值的 `submit-batch` 提升为正式的自适应 flush 机制，明确 `hard flush / soft flush / tail flush` 三层调度边界，并允许在固定参数版本验证有效后接入轻量在线观测策略。
- 将 resident/full-preload 查询路径中的 `probe_submit` 组织要求从“支持批量参数”升级为“必须按 pending work、队列压力、尾部状态和 probe 生命周期批量调度”，而不是继续依赖单一静态阈值。
- 明确 `submit-batch` 必须同时兼容两类 probe 上下文：
  - 固定 `nprobe` 的完整探测路径；
  - 在 `nprobe` 上限内按 CRC 判定提前终止的 early-stop 路径。
- 对 CRC early-stop 路径补充 stop-sensitive 约束：在正式 stop 判定前，系统必须允许执行 `stop-safe flush`；一旦判定停止，系统必须在退出 probe loop 前执行 `flush + drain`，而不是只在 query 末尾追加一次兜底提交。
- 将 Stage2 优化重点从 compute-ready layout 重构收缩为现有路径精修：保留当前 resident view 和 kernel 主体，正式要求 `collect` 支持 block-first 组织、`scatter` 支持 batch numeric classify / mask 化处理，并保持现有 recall 与 funnel 语义不变。
- 明确这一轮 Stage2 工作不引入新索引格式、不要求 compact layout rebuild，也不把 compute-ready resident view 作为主线能力。
- 补充 benchmark / experiment 契约，使本轮优化必须以固定 operating point 的多次重复实验、query-only 与 E2E 双口径、以及 submit / Stage2 子阶段归因字段来判断是否继续推进。
- 固化两条主实验基线，作为本轮 `submit-batch` 与 Stage2 follow-up 的正式验证口径：
  - 固定 probe 基线：`nprobe=64`，不启用 CRC early-stop，当前参考速度约 `0.5ms`
  - CRC early-stop 基线：`nprobe=256`、`crc-alpha=0.02`，启用 CRC early-stop，当前参考速度约 `1.0ms`

## Capabilities

### New Capabilities
- `adaptive-submit-batching`: 定义 resident 查询路径下基于 pending work、资源压力、尾部状态和 probe 生命周期的三层 flush 机制，以及后续轻量在线观测调度边界。

### Modified Capabilities
- `query-pipeline`: 查询主路径需要把本轮 follow-up 优化正式收敛到与固定 `nprobe` / CRC early-stop 兼容的 `submit-batch` 自适应调度、Stage2 collect/scatter 精修和稳定的阶段归因边界。
- `resident-query-hotpath`: resident/full-preload 路径需要把轻量候选提交组织从静态 batch 参数提升为可复用的自适应提交策略，并保持 thin-path 约束以及 early-stop 兼容性。
- `stage2-block-scheduling`: Stage2 block 调度需要支持从逐 survivor 线性拼装收敛到 block-first collect，并允许 scatter 在保持语义不变的前提下执行 batch numeric classify。
- `e2e-benchmark`: benchmark 输出和实验协议需要支持本轮 submit / Stage2 follow-up 的重复实验、变体对照和更明确的归因字段。

## Impact

- 主要影响代码：
  - `src/query/overlap_scheduler.cpp`
  - `include/vdb/query/overlap_scheduler.h`
  - `src/index/cluster_prober.cpp`
  - `src/simd/ip_exrabitq.cpp`
  - `benchmarks/bench_e2e.cpp`
  - `benchmarks/scripts/run_hotpath_experiments.py`
- 影响系统：
  - resident/full-preload 查询调度与提交路径
  - 固定 `nprobe` 与 CRC early-stop 两类 probe 生命周期下的 submit 行为
  - Stage2 `collect + kernel + scatter` 中的 block 调度与后处理
  - benchmark 归因字段与基于两条固定 operating point 的实验执行协议
- 不影响内容：
  - 不引入新的索引格式
  - 不要求重建现有索引
  - 不把 Stage2 compute-ready / compact layout 重构重新纳入本轮主线
