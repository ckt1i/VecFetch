## Context

`adopt-official-rabitq-one-plus-n` 已经把 RaBitQ 的 bit 语义改成官方 `1+n`，但 COCO100k 的 `total_bits=4, ex_bits=3` 查询结果显示，这一版 official ExData 的速度远低于预期。当前路径在 Stage2 touched block 上先把 packed 3-bit ExData 完整解码到 `uint8_t` scratch，再对 selected lanes 做 dot；其中 3-bit unpack 没有 SIMD fast path，导致 `official 1+3` 的 Stage2 明显慢于 legacy bits4。

本 change 的目标不是重新讨论 official estimator，而是把 `ex_bits=3` 的 Stage2 hot path 改成 direct compact IP：在 packed code 上直接计算 `ip_ex`，避免 query-time full-block materialization。根据用户要求，本轮必须实现并实测两个候选 layout：

```text
候选 A: 2-bit + 1-bit
  ex_code = low2 + 4 * high1
  每 64 维: low2 16B + high1 8B = 24B

候选 B: 1-bit + 1-bit + 1-bit
  ex_code = b0 + 2*b1 + 4*b2
  每 64 维: plane0 8B + plane1 8B + plane2 8B = 24B
```

两者空间相同，区别在 SIMD 计算形态。候选 A 与官方 RaBitQ library 的 3-bit compact code 思路一致；候选 B 是本项目可探索的 bitplane-only 变体，可能在 AVX512 mask-accumulate 场景下有竞争力，但 AVX2 复杂度更高。

## Goals / Non-Goals

**Goals:**

- 为 official `ex_bits=3` 实现 `2-bit + 1-bit` direct compact IP layout、writer/reader/resident view 和 Stage2 masked SIMD kernel。
- 为 official `ex_bits=3` 实现 `1-bit + 1-bit + 1-bit` direct compact IP layout、writer/reader/resident view 和 Stage2 masked SIMD kernel。
- 两种 layout 均必须与 scalar/reference official ExData dot 做 kernel parity，并与 v13 official decode path 做 E2E correctness 对拍。
- 在 COCO100k 上构建两个候选索引并运行同口径 query benchmark，至少覆盖 `nlist=2048, nprobe=64, topk=10, non_safeout_candidate_budget=400, queries=1000`。
- 根据同一轮结果选择更优 layout，将其作为 selected official `ex_bits=3` fast path；非优胜 layout 不作为默认 serving path。
- Benchmark 输出必须记录 layout key、是否 selected、Stage2 decode/input/output bytes、Stage2 kernel time、overall QPS、peak RSS 和 average rerank vectors。

**Non-Goals:**

- 不改变 official `1+n` 数学公式和 `ip_x0_qr + ip_ex + factor` 组合方式。
- 不改变 Stage1 FastScan、ConANN margin、SafeIn/SafeOut 语义或 raw-vector rerank 语义。
- 不修改 DiskANN、PQ、payload store、parquet、coarse routing 或数据集准备流程。
- 不要求旧二进制读取新的 optimized layout 索引。
- 不把 decode-to-scratch v13 删除；它仍作为验证和回退路径。

## Decisions

### Decision 1: 用 rebuild-required layout key 区分两种候选和最终 selected path

新索引 SHALL 在 metadata 中记录独立 layout key，例如：

- `official_1_plus_n_total4_ex3_split3_2plus1`
- `official_1_plus_n_total4_ex3_split3_bitplanes`
- `official_1_plus_n_total4_ex3_selected_direct`

实现可以使用新的 cluster store version，也可以在 v13 之后引入子 layout id；关键是 reader 必须显式区分旧 v13 generic bitstream 与本轮两个 optimized layout，不得启发式推断。

理由：当前 v13 generic packed bitstream 的解析语义与 direct compact layout 不同，混用会 silent corruption。显式 layout key 也让 benchmark 和论文结果能够复现具体路径。

备选方案是在 v13 内复用 `magnitude_bits=3` 并按 byte size 猜测 layout；该方案风险高，不采用。

### Decision 2: 候选 A 采用官方风格 `2-bit + 1-bit`

候选 A 每 64 维存 24 字节：前 16 字节是 low2 compact payload，后 8 字节是 high1 payload。查询 kernel 直接从 compact code 产生 0..7 的 ExData code 并 FMA 到 `ip_ex`。

推荐 batch kernel 结构：

```text
for dim_chunk in chunks_of_64:
  load q[0..63] once
  for lane in requested_lanes:
    load low2[16B], high1[8B]
    expand low2/high1 into four 16-lane SIMD vectors
    dot[lane] += dot(q, code)
```

理由：这与官方库 `packing_3bit_excode` / `ip64_fxu3_avx` 的数据访问模式一致，最容易达到预期性能，也最便于解释为 official-compatible optimized path。

备选方案是先 SIMD unpack 到 scratch 再 dot；这仍有 full-block materialization 和额外内存流量，不作为优化目标。

### Decision 3: 候选 B 采用三 bitplane direct IP，仅作为竞争布局

候选 B 每 64 维存三个 8 字节 plane：`b0/b1/b2`。查询 kernel 计算：

```text
ip_ex = sum(q * b0) + 2 * sum(q * b1) + 4 * sum(q * b2)
```

AVX512 可使用 mask load/add 或 maskz blend 形成 per-plane accumulation；AVX2 可先提供保守实现，再视 microbench 决定是否优化。

理由：三 bitplane layout 空间与 2+1 相同，概念简单，并可能在 sparse requested lanes 下减少 unpack/shift 工作。但它不是官方库的主布局，且 AVX2 实现复杂，因此必须通过 benchmark 决定是否保留。

### Decision 4: Stage2 query path 必须绕过 full-block decode

当 `ParsedCluster` 表明 layout 为 optimized direct layout 时，`ClusterProber` official 分支 SHALL 直接调用 direct compact masked kernel。该路径 MUST NOT 调用 `ExRaBitQDecodePackedBatchBlockMagnitudes`，也不得填充完整 `decoded_abs_scratch` 作为 hot path 输入。

旧 v13 generic bitstream 仍可走 decode-to-scratch path，用于回归对拍和 rollback。

### Decision 5: 比较和选择规则以端到端效果为准

两种 layout 都必须完成同口径 COCO100k benchmark。选择规则：

1. 先检查 correctness：kernel parity 通过，E2E recall 与 v13 official decode path 的差异在允许浮点误差内。
2. 在 correctness 都通过时，以 `avg_query_time_ms` / QPS 为主指标选择 winner。
3. 若两者 avg latency 差异小于 5%，选择 peak RSS 更低、实现更简单、跨 AVX2/AVX512 更稳的 layout。
4. 若两者都没有比 v13 official decode path 至少快 20%，则不得把任何一个标为 accepted main result；保留最快者为 experimental，并继续使用 legacy/旧 official 结果作为主实验候选。

理由：用户目标是先实现两种方案并“留下效果更好的那一个”。因此 final serving default 不能凭理论选择，必须由同口径数据决定。

## Risks / Trade-offs

- [Risk] 两种 layout 的 pack 语义与 scalar `ex_code` 不一致，导致 recall 或 ordering 回退。  
  Mitigation: 加 kernel reference tests，构建小样本 index 做 per-candidate `ip_ex` 对拍，再跑 COCO100k E2E recall。

- [Risk] 2+1 direct kernel 只优化 AVX512，AVX2 fallback 速度不足。  
  Mitigation: 先保证 AVX512 主环境达到目标；AVX2 fallback 必须正确且可显式标注性能未优化。

- [Risk] 1+1+1 实现成本高但最终不胜出。  
  Mitigation: 将其作为候选 layout 和 benchmark path 实现；若失败，移出默认 serving path，仅保留测试记录或删除热路径开关。

- [Risk] 新 layout 又引入索引格式碎片。  
  Mitigation: metadata 必须记录 layout key，最终只保留 selected layout 作为默认构建目标；非 selected layout 不进入主表。

- [Risk] Stage2 变快后瓶颈转移到 Stage1 或 submit/rerank。  
  Mitigation: benchmark 输出必须保留 Stage1/Stage2/probe_submit/fetch/rerank 分解，避免只看 overall latency。

## Migration Plan

1. 增加 optimized official 3-bit layout metadata 和 reader/writer 分支，旧 v13 generic bitstream 保持可读。
2. 实现候选 A `2-bit + 1-bit` pack、view、direct masked SIMD kernel 和 scalar reference。
3. 实现候选 B `1-bit + 1-bit + 1-bit` pack、view、direct masked SIMD kernel 和 scalar reference。
4. 为两个候选 layout 增加 unit/microbench correctness 与 latency 诊断。
5. 在 COCO100k 上分别构建两个 optimized official `total_bits=4, ex_bits=3` 索引。
6. 用相同 GT 和相同 query 参数运行两轮 E2E benchmark，输出 comparison summary。
7. 按选择规则把 winner 标记为 selected official 3-bit direct layout，并将 non-winner 从默认构建/查询路径中移除或降级为 experimental-only。
8. 若 winner 未达最低收益阈值，则不替换主结果；保留 v13 decode fallback 和 legacy signed-magnitude 主线。

## Open Questions

- 最终 selected layout 是否需要新 `.clu` major version，还是沿用 v13 并增加强制 layout id。
- 非优胜 layout 是完全删除 serving 代码，还是保留在测试/benchmark-only 开关中用于复现实验。
- AVX2 是否作为本轮硬性性能目标，还是只要求 correctness fallback。
