# RecordGate 结果—主张矩阵

日期：2026-07-18

## 判定口径

- `可直接主张`：已有严格对照或理论/单元测试足以支撑论文正文。
- `收窄后可主张`：机制事实成立，但效果范围或归因需要限定。
- `需补实验`：现有控制混入其他变量，不能单独归因。
- `不应主张`：现有严格实验与该结论相反。

## 主张矩阵

| ID | 候选主张 | 当前证据 | 判定 | 正文安全表述 | 缺口/动作 |
|---|---|---|---|---|---|
| C0 | SafeOut 保证全库 exact top-k | SafeOut 仅作用于 ANN/IVF 已生成候选，不能恢复 candidate generation 漏召 | 不应主张 | `the verification guarantee is scoped to the ANN-generated candidate set; candidate-generation recall is orthogonal` | 写入 correctness contract，并把 candidate-generation recall 单列 |
| C1 | 动态 SafeOut 能减少不必要的 raw-vector verification | NoSafeOut 在 ESCI/MSMARCO 上使 latency 分别约恶化 37.3x/65.8x，读取与 rerank 候选增加约两个数量级 | 收窄后可主张 | `conditional on the configured query-level bound event, bound-guided pruning substantially reduces verification work relative to verify-all` | 冻结联合置信/failure-budget 口径；报告 recall 与读取量 |
| C2 | SafeOut 是无误差的确定性剪枝 | 当前 RaBitQ epsilon/bound 使用概率/校准置信口径 | 不应主张 | `under the configured confidence policy, the exclusion certificate is monotone as the frontier tightens` | 全文删除 `zero-error`、`never misclassifies` 等绝对措辞 |
| C3 | 相比固定 rerank depth，RecordGate 在相同 recall 下读取更少 | 当前 NoSafeOut 不是 fixed-depth baseline | 需补实验 | 暂写为问题动机：`static budgets cannot adapt verification work to query- and candidate-specific bound evidence` | 增加 2 个代表数据集的 fixed-depth matched-recall frontier |
| C4 | SafeIn 能稳定提前读取最终 top-k payload | eager、cold-prefix、SE 三组严格实验均未形成稳定收益，且存在大量无效读取 | 不应主张 | 不进入方法与贡献；可在 appendix/report 中作为 negative design study | 默认关闭；论文删除 SafeIn-aware credit、tail extension 和投机 payload prefetch |
| C5 | SafeOut 与 SafeIn 在在线扫描中具有相同的证书性质 | SafeOut frontier 单调收紧；early inclusion condition 不是最终 membership 的扫描单调证书 | 不应主张 | `exclusion is online-monotone; early inclusion is not a scan-monotone final-membership certificate` | 在设计选择中用一段简短论证解释为何只保留 SafeOut |
| C6 | bounded span 合并显著减少读取请求 | NoSpan→GV：ESCI/MSMARCO requests -55.20%/-68.05%，QPS +1.82%/+4.81% | 可直接主张 | `spans reduce admitted vector-read requests under a per-span real-vector-byte amplification bound` | 复用 paired 结果；分别报告 vector useful/physical bytes；该约束不覆盖 final payload fetch |
| C7 | exact planner 在所定义的一维模型中给出全局最优分段 | 43/43 tests；与独立 O(n^2) oracle 分段一致 | 可直接主张 | `exact for the fixed-order, same-tile interval-partition model` | 明确模型边界；不能推广为一般 I/O planning 全局最优 |
| C8 | O(n log n) exact planner 比 greedy 吞吐更高 | GE→GV QPS 在 ESCI/MSMARCO 为 -0.89%/-0.62%，虽 requests 略减 | 不应主张 | `matches the frozen exact objective in O(n log n) worst-case time with about 0.06 ms/query measured overhead` | 把 exact 的价值定位为可审计最优性，而非 QPS 胜出 |
| C9 | probing-time pipeline 的收益来自 CPU/I/O overlap | Full vs NewNoPipeline 同时改变 async batching、reuse、tail、SafeIn 和 final reads | 需补实验 | 暂写机制，不报独立收益 | P1 补 `NoOverlapAsyncFinal`；若要保留 headline overlap claim 则升级为 P0 |
| C10 | inline payload 的 natural reuse 减少 final materialization I/O | ESCI/MSM requests -8.012%/-10.441%，bytes -1.228%/-1.143%；QPS -0.162%/+0.421%；COCO/Vox 无 view | 收窄后可主张 | `when admitted vector spans cover valid inline extents that are later consumed by the final materializer, RecordGate reduces missing-payload I/O` | 区分 eligible/covered/registered/consumed bytes；不声称普遍加速 |
| C11 | adaptive record layout 本身普遍提高 QPS | NoCombine/Combined 历史结果混合 planner、reuse、cache 与 layout；COCO 存在反向条件 | 需补实验/收窄 | `the layout enables bounded span cofetch and avoids a separate payload read for covered bytes` | 若时间不足，只主张 enabling property 和 I/O reduction，不主张 universal QPS |
| C12 | 完整系统优于传统分阶段执行 | 已有 end-to-end 数据，但各 baseline 的功能和缓存口径需逐项审计 | 收窄后可主张 | `RecordGate coordinates candidate verification and record materialization across the vector/record boundary` | 主表逐个列出 recall、QPS、p99、requests、bytes；避免把所有收益归于单组件 |

## 最终保留的论文主张

1. **Confidence-budgeted verification control**：利用概率距离界和不断收紧的 conservative top-k upper frontier，单调地排除无需读取 raw vector 的候选；保证限定于 ANN 候选集及配置的 query-level bound event。
2. **Amplification-bounded span execution over reusable records**：对 frozen admitted vector-read set 做固定顺序、同 tile 的一维最优分段；span 已覆盖且最终被消费的合法 inline payload 字节被自然复用。

Evaluation 作为 RQ 而不是第三条技术贡献，分别量化 bound-guided pruning、span request reduction、layout--span interaction 和 natural payload reuse。pipeline overlap 只有在公平 NoOverlap 对照完成后才作为独立结果。
