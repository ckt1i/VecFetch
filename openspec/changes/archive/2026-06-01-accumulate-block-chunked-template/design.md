## Context

`simd::AccumulateBlock` 是 FastScan Stage1 估计路径的核心 SIMD kernel，当前由 `rabitq_estimator` 在 `EstimateDistanceFastScan` 和 `EvaluateStage1FastScan` 中调用。现有 AVX512 实现每个主循环迭代处理 16 维，AVX2 实现每个主循环迭代处理 8 维；循环本身按 SIMD 最小步长推进，能够支持不同维度，但没有针对常见 `n*32` / `n*64` 工作点做 chunk 级展开。

后续数据集维度不固定，除 MSMARCO 的 768 维外，还可能包括 512、1024 或其他 `n*32` / `n*64` 维度。因此本 change 不选择只为 `512/768/1024` 写固定模板，而是把优化边界提升为通用 chunk specialization。

## Goals / Non-Goals

**Goals:**

- 为 `AccumulateBlock` 增加覆盖任意 `n*64` 和 `n*32` 的内部 fast path。
- 保持 public API、packed code 布局、packed LUT 布局和 `result[32]` 语义不变。
- 保持 Stage1 / Stage2 / rerank / recall / top-k 语义不变。
- 控制二进制体积，只实例化少量 chunk 模板，而不是为大量固定维度生成独立函数。
- 为 AVX512 和 AVX2 都提供对应结构；scalar fallback 保持可用。
- 增加 correctness 测试和端到端 benchmark/perf 验证。

**Non-Goals:**

- 不实现只面向 `512/768/1024` 的固定维度完全展开版本。
- 不修改 FastScan LUT 生成、packed code 存储格式或 query prepare 逻辑。
- 不改 Stage1 mask/classify、Stage2、rerank 或 IO pipeline。
- 不新增 benchmark JSON 字段或用户可见 CLI。
- 不把该 change 与 coarse routing 参数优化混合归因。

## Decisions

### Decision 1: 保留 `AccumulateBlock` public API，只做内部 dispatch

继续使用现有签名：

```cpp
void AccumulateBlock(const uint8_t* packed_codes,
                     const uint8_t* lut,
                     uint32_t* result,
                     Dim dim);
```

内部按维度分派：

```cpp
if (dim % 64 == 0) {
    AccumulateBlockChunked<64>(packed_codes, lut, result, dim);
} else if (dim % 32 == 0) {
    AccumulateBlockChunked<32>(packed_codes, lut, result, dim);
} else {
    AccumulateBlockGeneric(packed_codes, lut, result, dim);
}
```

理由：

- 调用方无需改动，风险最小。
- `n*64` 先匹配，768、512、1024 等维度都会进入 64-dim chunk path。
- `n*32` 非 64 倍数维度仍有 fast path。
- 其他合法维度继续保持现有行为。

备选方案是新增 `AccumulateBlockDim32` / `AccumulateBlockDim64` public API，但这会把维度分派泄露给业务层，不符合当前 SIMD kernel 封装方式。

### Decision 2: 用 chunk 模板化，而不是逐维度模板化

首版只实例化：

- `AccumulateBlockChunked<64>`
- `AccumulateBlockChunked<32>`

AVX512 中，一个 `Step16` 对应当前一次主循环迭代；`<32>` chunk 调用两次 `Step16`，`<64>` chunk 调用四次 `Step16`。AVX2 中，一个 `Step8` 对应当前一次主循环迭代；`<32>` chunk 调用四次 `Step8`，`<64>` chunk 调用八次 `Step8`。

理由：

- 既覆盖任意 `n*32` / `n*64`，又避免为大量 `dim` 生成独立函数。
- chunk 内 step 数固定，编译器可以展开和调度 chunk 内指令。
- 外层循环次数从按 SIMD 最小步长推进减少到按 32/64 维 chunk 推进。

备选方案是 `AccumulateBlockDim<K>` 对所有常见 K 维度实例化。它可能获得更强 unroll，但会带来代码体积和维护成本，适合作为 perf 证明必要后的二阶段优化。

### Decision 3: 先抽取 step helper，再复用现有 reduction

AVX512 抽取：

```cpp
VDB_FORCE_INLINE void AccumulateStep16Avx512(
    const uint8_t*& packed_codes,
    const uint8_t*& lut,
    __m512i accu[2][4],
    const __m512i lo_mask);
```

AVX2 抽取：

```cpp
VDB_FORCE_INLINE void AccumulateStep8Avx2(
    const uint8_t*& packed_codes,
    const uint8_t*& lut,
    __m256i accu[2][4],
    const __m256i lo_mask);
```

step helper 必须保持与当前循环体完全一致的 pointer increment：

- AVX512 每个 `Step16`：`packed_codes += 64`，`lut` 消耗两个 64-byte LUT plane。
- AVX2 每个 `Step8`：`packed_codes += 32`，`lut` 消耗两个 32-byte LUT plane。

reduction / combine `result[32]` 逻辑首版尽量复用现有代码，避免同时改动 accumulation 和 reduction 两个风险点。

### Decision 4: fallback 保留现有 generic 实现

`dim` 不满足 `n*32` / `n*64` 时继续走 generic 路径。generic 路径也作为 correctness reference 的重要对照。

理由：

- 避免对非主工作点引入回归。
- 允许测试中直接比较 chunked path 与 reference path。
- 保留后续 rollback 能力。

## Risks / Trade-offs

- Pointer increment 错误 → 通过对多维度随机 packed codes / LUT 做 bit-exact reference 测试覆盖，特别覆盖 32、64、96、544、768、1024。
- LUT plane layout 破坏 → step helper 必须从现有 loop body 机械拆分，先不重排 LUT load 顺序。
- 性能收益低于预期 → chunk 模板主要减少 loop/control overhead，若热点主要是 shuffle/add 指令本身，收益可能只有 `0.03~0.06 ms/query`。若 perf 仍显示 `AccumulateBlock` 占比高，再考虑第二阶段固定 block-count specialization。
- 代码体积增加 → 首版只实例化 `<32>` 和 `<64>`，不做全维度模板表。
- AVX512 与 AVX2 行为不一致 → 两条路径都必须有相同维度分派策略和相同 reference 测试。
