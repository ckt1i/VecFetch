# RecordGate 论文决策记录：SafeIn 移除

日期：2026-07-18

## 冻结决策

- SafeIn 从论文 abstract、contributions、method 主路径、主图、默认配置和主实验移除。
- eager full-record prefetch、external/cold payload prefetch、SafeIn-aware span credit 和 tail extension 在 paper config 中关闭。
- natural payload reuse 保留：它只复用 mandatory vector span 已经覆盖的 inline payload，不依赖 SafeIn membership prediction。
- SafeIn 代码暂不物理删除；提交后作为 inactive-path cleanup 单独处理。

## 冻结术语

- `SafeOut` 论文名优先写为 `Bound-Guided Verification Control` 或 `Monotone Exclusion Frontier`。
- `span prefetch` 改为 `amplification-bounded span execution/cofetch`。
- `payload prefetch` 仅用于真正额外发起的 speculative I/O；natural reuse 不使用该词。
- `exact O(n log n)` 必须附带限定：fixed-order、same-tile、fixed-admission interval partition with a frozen lexicographic objective。

## 冻结主线

1. Bound evidence 决定哪些 raw vectors 仍需 verification。
2. Mandatory reads 被组织成 bounded spans 并异步执行。
3. Span 已覆盖的 inline payload 建立 reusable view；final top-k 后只补缺失数据。

## 禁止的论文主张

- SafeOut 无条件零误判或保证全库 exact top-k。
- 传统 fixed rerank budget 没有理论依据、必然误差大。
- exact planner 比 greedy 吞吐更高。
- adaptive layout 或 natural reuse 在所有数据集普遍提高 QPS。
- 所有 probing 期间提前发出的 raw-vector reads 最终都必需。

## 待完成的 P0

- fixed-depth matched-recall frontier；
- `Combined/NoCombine × NoSpan/GE` layout--span interaction；
- SafeIn-free correctness/configuration audit；
- same-path `NoOverlapAsyncFinal` 降为 P1，除非保留 headline overlap claim；
- layout--span natural reuse 的最小交互/coverage 审计；
- query-level bound/candidate-set correctness contract。
