## Context

当前 resident/full-preload 主路径已经具备较完整的分段统计和实验脚本，且最近一轮固定 operating point 对照已经给出比较清晰的结论：

- `submit-batch-32` 相比当前基线有小而稳定的收益；
- `window-read` 与 `no-resident` 没有打开新的主要收益面；
- Stage2 compute-ready / parallel-view 重构在当前实现上多次尝试后都没有稳定收益；
- 真正仍可继续压缩的部分，集中在 query 调度骨架和 Stage2 现有路径中的标量组织成本。
- 本轮 follow-up 的正式验证口径固定为两条主基线：
  - 固定 probe 基线：`nprobe=64`，不启用 CRC early-stop，当前参考速度约 `0.5ms`
  - CRC early-stop 基线：`nprobe=256`、`crc-alpha=0.02`，启用 CRC early-stop，当前参考速度约 `1.0ms`

这意味着本轮 change 不应再沿着“更大 resident layout 重构”继续扩大范围，而应把已经得到正反馈的 `submit-batch` 细化为正式机制，同时把 Stage2 优化收敛到不改索引格式的 `collect/scatter` 精修。

本轮设计同时受两个约束：

1. 必须保持 recall、SafeIn/Uncertain funnel 语义、rerank 语义和最终排序语义不变。
2. 必须继续维持现有 benchmark 归因边界，使每一轮 follow-up 优化都能和当前 0.5ms 左右的稳定基线做对照。
3. 必须同时兼容两类 probe 生命周期：固定 `nprobe` 的完整探测，以及在 `nprobe` 上限内由 CRC 驱动的 early-stop 探测。

## Goals / Non-Goals

**Goals:**
- 将 `submit-batch` 从静态阈值选项提升为 resident hot path 下的正式三层 flush 机制。
- 让 `submit-batch` 与固定 `nprobe` / CRC early-stop 两类 probe 模式都保持语义兼容，而不是只在完整探测路径下成立。
- 为 `submit-batch` 提供固定参数版和在线观测版的清晰升级路径，并优先落地低风险的固定参数版。
- 将 Stage2 优化主线收敛到当前 block scheduling 路径上的两个热点：
  - collect 从逐 survivor 线性拼装收敛到 block-first 组织；
  - scatter 从逐 lane 标量 classify 收敛到 batch numeric classify + mask 压缩写回。
- 固化实验协议，使 `submit-batch` 与 Stage2 follow-up 的每次推进都必须带有稳定的重复实验与 query-only / E2E 双口径验证。
- 将实验比较固定到上述两条 operating point，避免后续分析混入参数漂移。

**Non-Goals:**
- 不重新引入 Stage2 compute-ready resident view、compact layout rebuild 或新索引格式升级。
- 不要求本轮直接实现在线自适应 submit 调度；在线版只定义边界和升级条件。
- 不重做 Stage2 kernel 主体的存储消费模型，也不把目标扩展到新的 batch-block 布局。
- 不把 rerank、coarse top-n 或 prepare 拆分纳入本轮主设计，除非作为后续实验决策结果单独立项。
- 不改变 CRC 判定公式本身；本轮只调整 stop 判定前后 submit/flush/drain 的时序约束。

## Decisions

### 1. `submit-batch` 采用三层 flush 机制，而不是继续暴露单一固定阈值

本轮把 resident 提交路径的调度决策分成三层：

- `hard flush`：资源边界触发，必须立即提交；
- `soft flush`：达到目标批量或间隔上限时提交；
- `tail flush`：剩余 cluster 已不足以攒到理想批量时提前收尾。

这样做的原因是，最近实验已经说明固定 `submit-batch=32` 有收益，但收益不大，说明问题不只是“阈值大小”，而是 flush 时机本身仍偏粗糙。三层机制允许在不改变结果语义的前提下，同时处理：

- 中段批量不足时的提交骨架成本；
- 尾部小批次拖延；
- ring / buffer 压力导致的被动 flush。
- 与 fixed `nprobe` / CRC early-stop 两类 probe 生命周期对齐的提交控制。

备选方案与取舍：

- 方案 A：继续只暴露一个静态 `submit_batch_size`。
  好处是实现最小，但不能解决尾部和资源压力下的低效 flush。
- 方案 B：直接上完全在线自适应调度。
  理论上更灵活，但调试面和解释成本都更高。
- 方案 C：先固定参数三层机制，再把在线观测作为第二层升级。
  这是当前收益、可验证性和实现风险之间最稳的路径。

### 2. `submit-batch` 需要区分 normal probe 与 stop-sensitive probe 上下文

三层 flush 本身不足以覆盖 CRC early-stop 路径，因为 early-stop 的 stop 判定发生在 probe 循环内部，而不是 query 全部 cluster 结束之后。因此本轮将 submit 调度再分成两类上下文：

- `normal probe context`
  - 适用于固定 `nprobe` 路径，以及 CRC 尚未接近 stop 的常规推进阶段；
  - 主要依赖 `hard flush / soft flush / tail flush`。
- `stop-sensitive context`
  - 适用于每次 cluster-end 即将执行 CRC stop 判定，或当前 query 已接近 stop 的阶段；
  - 需要在 stop 判定前提供 `stop-safe flush`；
  - 一旦决定停止，需要在 break probe loop 前执行 `flush + drain`，而不是仅在 query 末尾追加兜底 flush。

这样做的原因是：

- 固定 `nprobe` 路径只需保证中途 batching 不损伤 overlap，循环尾部做 `final flush` 即可兜底；
- CRC early-stop 路径如果只在最后强制提交，会让 stop 判定看到过旧的 pending 状态，导致 stop 滞后，甚至让 probe 生命周期与未提交请求生命周期错位。

备选方案与取舍：

- 方案 A：只在 query 末尾增加一次强制提交。
  对固定 `nprobe` 是必要兜底，但不足以保证 CRC early-stop 的判定新鲜度。
- 方案 B：每个 cluster-end 都完整 flush + drain。
  正确性直接，但会显著削弱 overlap。
- 方案 C：仅在 stop-sensitive 上下文下执行 `stop-safe flush` 和 break 前 `flush + drain`。
  兼顾 early-stop 正确性与常规 batching 收益，是当前最合理的折中。

### 3. 固定参数版优先，在线观测版只作为升级路径

固定参数版采用清晰的初始参数：

- `target_batch = min(32, sq_budget)`
- `interval_limit = 4`
- `tail_window = 3`
- `tail_min_batch = 8`

在线观测版只定义轻量边界：

- 每 query 维护 `ema_req_per_cluster`
- 允许据此动态调整 `target_batch` 与 `interval_limit`
- 允许基于 `expected_future_req` 触发 forecast-driven tail flush

这样分层的原因是，当前代码库已经多次经历“理论更合理但实测收益差”的情况。固定参数版能先证明这条主线是否成立；只有当固定参数版拿到稳定正收益时，在线版才值得继续投入。

### 4. Stage2 不再追求 layout 重构，而是明确收敛到 `collect + scatter`

本轮对 Stage2 的核心判断是：

- kernel 本身不是唯一瓶颈；
- layout 重构已经证明实现风险高、收益不稳定；
- 当前路径最明显的额外成本来自 block collect 的组织方式和 kernel 后 scatter 的逐 lane 标量处理。

因此本轮 Stage2 设计只做两件事：

1. `collect` 改成 block-first
   - 从“每个 survivor 都线性扫描 `stage2_blocks[]`”收敛到“先形成 per-block mask / slot，再批量填充 block scratch”。
2. `scatter` 改成 batch numeric classify
   - 用 8-lane SIMD 先完成 `ip_raw -> ip_est -> est_dist_s2 -> classify mask`
   - 最后只把 surviving lanes 做标量压缩写回。

备选方案与取舍：

- 方案 A：继续做 compute-ready / compact view。
  已多次失败，当前不再是主线。
- 方案 B：只继续抠 kernel 微优化。
  容易被 collect/scatter 标量骨架吞掉收益。
- 方案 C：保持现有 resident view 和 kernel 入口不变，集中压缩 collect/scatter。
  最符合当前 profiling 证据，也最不容易引入 recall 风险。

### 5. Stage2 scatter SIMD 只向量化数值分类，不强求 SIMD 写回

scatter 的设计明确拆成两段：

- `numeric stage`
  - 8-lane 计算 `ip_est`
  - 8-lane 计算 `est_dist_s2`
  - 8-lane 生成 `SafeIn / SafeOut / Uncertain` mask
- `compact writeback stage`
  - 使用 mask 做 `popcount`
  - 标量写回 surviving lanes 到 `CandidateBatch`

这样做是因为：

- `CandidateBatch` 的写回本身带有条件压缩和计数更新；
- 真正适合 SIMD 的是 scatter 的数值链，而不是最后几条条件写入；
- 该设计可以最大化复用现有 `CandidateBatch` 与 funnel 语义。

### 6. benchmark 与实验协议要成为本轮 change 的正式组成部分

本轮已经有了专门的实验脚本和一轮基础对照结果，因此 design 明确要求：

- 后续 `submit-batch` / Stage2 follow-up 必须使用固定 operating point 重复实验；
- 主 operating point 固定为两组：
  - 固定 probe：`nprobe=64`，不启用 CRC early-stop，参考速度约 `0.5ms`
  - CRC early-stop：`nprobe=256`、`crc-alpha=0.02`，参考速度约 `1.0ms`
- 必须同时保留 query-only perf 与 full E2E 结果；
- 必须导出足够解释 flush / collect / scatter 变化的统计字段；
- Stage2 compute-ready 只作为后验对照，不进入主实验组。

这项决策的目的是避免再次进入“大改实现，但 benchmark 只给总时间，无法解释为什么没赢”的状态。

## Risks / Trade-offs

- `[三层 flush 过于保守]` → 可能只拿到很小收益。缓解方式是固定参数版先落地并保留同 operating point 对照，不在第一轮追求复杂自适应。
- `[CRC early-stop 与 batching 冲突]` → 在 stop-sensitive 上下文下显式加入 `stop-safe flush` 与 break 前 `flush + drain`，而不是只依赖 query 尾部兜底提交。
- `[在线观测调度增加解释成本]` → 将其明确为第二阶段升级，仅在固定参数版证明有效后启用。
- `[Stage2 collect 重组导致控制流更复杂]` → 先限定在 `v11` / resident 主路径的局部 fast path，不反向污染旧路径。
- `[Stage2 scatter SIMD 改动误伤语义]` → 保持 `batch.est_dist`、SafeIn/Uncertain 分类结果和最终 funnel 语义与参考路径一致，并用 query-only benchmark 回归。
- `[统计字段继续不足]` → benchmark 明确把 submit flush 次数、请求规模和 Stage2 子阶段归因作为正式输出要求，避免后续工作无法解释收益。

## Migration Plan

1. 先落 proposal/spec 中定义的实验协议与字段约束，保证后续实现围绕两条固定基线执行。
2. 第一阶段实现固定参数版三层 flush，并同时补齐 fixed `nprobe` / CRC early-stop 的 stop-sensitive flush 约束。
3. 第二阶段实现 Stage2 collect block-first 组织与 scatter numeric SIMD。
4. 分别在 `nprobe=64` 基线和 `nprobe=256 + crc-alpha=0.02` 基线上做重复实验，若固定参数版 `submit-batch` 收益稳定，再进入在线观测版。
5. 如果任一阶段收益低于噪声区间，保留实验结论并暂停该方向，而不是继续扩大实现面。

回滚策略：

- `submit-batch` 自适应若收益不稳，可回退到已验证的固定 `submit-batch-32` 或当前静态行为。
- Stage2 collect/scatter 精修若引入语义风险，可保持原 kernel 和原 collect/scatter 逻辑，同时保留 benchmark 字段和实验脚手架。

## Open Questions

- 固定参数版是否需要把 `target_batch` 上限直接固定为 32，还是保留对 ring 容量的更紧约束？
- 在线观测版是否需要同时维护 `ema_bytes_per_cluster`，还是 `ema_req_per_cluster` 已足够支撑第一版调度？
- Stage2 scatter SIMD 是否只对 `kStage2BatchSize=8` 提供专门路径，还是需要为将来更宽 batch 保留接口抽象？
- 对 collect 来说，第一版是“block-local fixed map”更稳，还是“per-block lane mask + delayed fill”更清晰？
