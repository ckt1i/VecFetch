## 1. 基线与画像确认

- [x] 1.1 重新跑 resident COCO benchmark，开启 `--fine-grained-timing 1`，记录当前的 `stage2_kernel`、`stage2_kernel_sign_flip`、`stage2_kernel_abs_fma`、`stage2_kernel_tail` 和 `stage2_kernel_reduce`。  
  已完成：已记录多轮 baseline，并用于判断第一阶段 lane-batched scratch 方案的退化原因。
- [x] 1.2 在改 kernel 之前，确认低开销 benchmark 的 recall 和 latency 画像仍然和当前基线一致。  
  已完成：低开销口径 baseline 已确认，作为后续第一阶段失败对照。

## 2. 第一阶段：kernel 内 lane-batched scratch

- [x] 2.1 在 `IPExRaBitQBatchPackedSignCompact()` 内引入 kernel-local lane-batched scratch，不改现有 compact-block layout。  
  已验证：2-lane / 4-lane / 16-dim subblock 版本都实现并跑通，但低开销 benchmark 退化。
- [x] 2.2 把 `64/32/16` 路径的 workflow 拆成 `sign prepare phase` 和 `abs consume phase`。  
  已验证：结构上拆分完成，但显式 scratch 物化带来额外 store/reload 开销。
- [x] 2.3 第一版 lane batching 先限制在 `2-lane` 或 `4-lane`，不要直接扩到全部 `8 lanes`。  
  已验证：2-lane / 4-lane 都试过，未达到性能门槛。
- [x] 2.4 在改 kernel 的同时，保持现有 compact-block layout 和 Stage 2 classification semantics 不变。  
  已完成：语义正确，但性能未达标。
- [x] 2.5 明确 `signed_q_cache` 或等价中间表示作为 `sign prepare boundary` 和 `abs consume boundary` 之间的稳定接口。  
  已验证：接口定义成立，但显式大 scratch 方案整体不可行。

## 3. 第一阶段验证与归因

- [x] 3.1 重新跑 resident benchmark 的 `--fine-grained-timing 1`，确认 Stage 2 kernel 子段加总仍然回到 `stage2_kernel`。  
  已完成：归因口径有效，但子项未带来性能收益。
- [x] 3.2 重新跑低开销 benchmark，确认 recall 和总 query time 稳定或改善。  
  已完成：语义稳定，但 query time 无改善。
- [x] 3.3 确认第一阶段至少改善了 `stage2_kernel_sign_flip` 或 `stage2_kernel_abs_fma` 之一，再决定是否进入第二阶段。  
  已完成：未满足改善条件，因此第一阶段判断为不可行。
- [x] 3.4 在一小段非 `2^n` 的数据集样本上验证 compact kernel，确认 kernel 改动不依赖固定维度。  
  已完成：功能正确，但仍不具备性能优势。

## 4. 第一阶段后续细化

- [x] 4.1 把 `IPExRaBitQBatchPackedSignCompact()` 热循环里的 sign chunk 微拷贝换成 `64/32/16` 路径里更直接的 load。  
  已试探：未带来收益。
- [x] 4.2 减少同一组 loaded query slices 上重复的 per-lane flip-mask 生成或等价重复 query-flip 工作。  
  已试探：未带来收益，且引入额外复杂度。
- [x] 4.3 重新跑细粒度 profiling，确认 `stage2_kernel_sign_flip` 下降，同时分类和 recall 不回归。  
  已完成：未下降，因此放弃该方向作为主线。

## 5. 第一阶段 abs/FMA 优化

- [x] 5.1 收紧 `LoadAbsMagnitude16Avx512()` 的消费链，以及 compact kernel 周围的 abs unpack/convert 路径。  
  已试探：未带来收益。
- [x] 5.2 只有在 `5.1` 之后 abs/FMA 仍然是主导项时，才重新审视 bias accumulation。  
  已完成：未满足继续深挖条件，bias 不再作为第一阶段主线。
- [x] 5.3 重新跑细粒度 profiling，确认 `stage2_kernel_abs_fma` 下降，同时 kernel 子段仍然能回收到总 Stage 2 kernel 归因。  
  已完成：未下降，因此第一阶段 abs/FMA 路径终止。
- [x] 5.4 除非 profiling 证明 layout 本身才是瓶颈，否则保持 lane/block 迭代顺序和 compact-block layout 不变。  
  已完成：layout 未被证明是瓶颈，保持不变。

## 6. 第二阶段：preload-time transcode 候选方案

- [x] 6.1 仅在第一阶段确认 lane-batched kernel 有收益之后，再设计 preload/load 阶段的 parallel-friendly Stage2 view。
- [x] 6.2 为第二阶段定义 parallel-friendly resident view 的数据结构，目标是更适合 lane-batched kernel 消费。
- [x] 6.3 在 cluster load / preload 阶段构建该 view，但不改变盘上 compact-block 格式。
- [x] 6.4 第一版第二阶段先保留双视图：original compact view + parallel-friendly resident view。
- [x] 6.5 为第二阶段新增观测字段，至少包含 `view_build_ms` 和 `view_bytes`。
- [x] 6.6 比较“第一阶段 kernel”与“第二阶段 resident view + 新 kernel”的 query-time 收益与 load-time 成本，再决定是否保留第二阶段方案。
- [x] 6.7 确保第二阶段只改变 `sign prepare boundary` / `abs consume boundary` 的输入表示，而不重新定义 kernel 内部语义。

## 7. 第三阶段：parallel view + 2-lane batched kernel

- [x] 7.1 在第二阶段 parallel-friendly resident view 上，设计 `2-lane` 的 query-time batched kernel，不回到显式大 scratch。
- [x] 7.2 让第三阶段 kernel 保持 `sign prepare boundary` / `abs consume boundary` 不变，但把消费单位从单 lane 提升到 lane batch。
- [x] 7.3 第三阶段优先把中间态限制在寄存器驻留或极小局部状态中，避免 query-time store / reload。
- [x] 7.4 先做方案 A，只重排 `sign prepare` 的共享粒度，让多个 lane 尽可能共享同一轮 sign 处理，abs consume 暂时保持不变。
- [x] 7.5 在方案 A 证明有效后，再做方案 B，把 `sign + abs` 一起纳入 lane 共享调度。
- [x] 7.6 只有在方案 A 和方案 B 都表明共享粒度重排有效时，才考虑将第三阶段放大到 `4-lane` batching。
- [x] 7.7 重新跑 resident benchmark 的 `--fine-grained-timing 1`，确认 `stage2_kernel_sign_flip` 或 `stage2_kernel_abs_fma` 至少有一项继续下降。
- [x] 7.8 重新跑低开销 benchmark，确认第三阶段不会破坏 recall，也不会让 `avg_query` 回退。

## 状态说明

当前 change 的主路线已经调整为两阶段：
- 第一阶段先做不改 layout 的 kernel 内 lane-batched scratch。
- 第二阶段只有在第一阶段证明收益后，才推进 preload-time transcode。

此前“sign-path / abs-path 小粒度微调”的直接收益有限，因此后续实现不应再把局部微调作为主方案。
补充状态：
- 第一阶段已经做过第一轮直接实现试探：
  - `2-lane` 整段 slice scratch
  - `4-lane` 整段 slice scratch
  - `2-lane` 的 16-dim subblock 分阶段
- 这三版都保持了语义正确，但低开销 benchmark 均退化，因此当前任务 `2.1` 到 `2.5` 不应按这三种直接实现方式勾选完成。
- 下一步应在两条路线中二选一：
  - 继续探索不显式物化大块 `signed_q_cache` 的更窄第一阶段实现
  - 或转向第二阶段原型，验证 preload-time transcode 是否能够把第一阶段中暴露出的 query-time scratch 成本前移出去
- 第二阶段原型已经实现并测量：
  - 低开销口径 `probe_stage2` 小幅下降，`avg_query` 基本持平
  - `parallel_view_build_ms` 与 `resident_parallel_view_bytes` 作为新增成本已经被观测
  - 细粒度口径下的新 kernel 仍未形成预期中的 lane-batched 并行收益
 - 第三阶段应建立在第二阶段的 parallel-friendly resident view 上，优先尝试 `2-lane` register-resident batching，而不是回到第一阶段的显式 scratch。
- 第三阶段的实现顺序已经细化为：
  - 先做方案 A，只重排 `sign prepare` 的共享粒度
  - 再做方案 B，把 `sign + abs` 一起纳入 lane 共享调度
  - 只有在这两步成立后，才考虑放大到 `4-lane`
- 第三阶段已经完成实现验证：
  - `2-lane` 和 `4-lane` 的 lane-sharing 版本都能编译并跑通
  - 低开销口径下未获得稳定收益，`probe_stage2` 基本持平或略差
  - 细粒度口径下 `stage2_kernel_sign_flip` / `stage2_kernel_abs_fma` 也未形成预期下降
  - 因此第三阶段当前应视为“实现完成但未达性能门槛”，不再继续向更宽 lane batching 放大
- 这一结论意味着第三阶段相关任务在工程上可以视为已执行完毕，但性能目标未达成；后续如继续优化，应从更深层的 kernel 消费结构或进一步的表示设计入手，而不是继续扩大第三阶段批宽。
