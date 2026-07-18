# 传统 SafeIn Prefetch 严格 NoReuse 消融结果

日期：2026-07-18

## 结论

关闭 `--vec-span-payload-reuse` 后重新运行的严格消融表明：传统 eager SafeIn
full-record prefetch 在四个数据集上均降低 QPS，20 个同-repetition 配对全部为负。
即使采用每个数据集最低 NoSafeIn QPS 的固定基线，20 个 SafeIn repetition 仍全部
为负。因此旧的 `SE,rho=0.1,reuse=1` 表不再作为严格 SafeIn-prefetch 消融使用，
本报告是后续论文的唯一有效口径。

## 实验合同

两组均固定：

- combined inline record layout；
- `GE,rho=0,alpha=3/2,tail=0`；
- `vec_span_payload_reuse=0`、payload views/reuse/credit 全部为 0；
- `topk=100,nprobe=96`，500 queries，5 次交替顺序配对；
- measured run 使用 `payload-cache-mode=drop-before-queries`。

唯一处理变量为：

- `SafeInPrefetch`：`materialization=eager,safein_as_vec_only=false`；
- `NoSafeInPrefetch`：`materialization=late,safein_as_vec_only=true`。

不能继续使用 `SE,rho>0`，因为 SafeIn-aware admission 的 credit 只有在 payload
实际复用时才合法；`reuse=0` 下使用该组合会违反方法合同并被当前实现 fail-fast。

## 五次同-repetition 配对

下表为 `SafeInPrefetch / NoSafeInPrefetch - 1`；QPS 正值更好，p99 负值更好。

| 数据集 | SafeIn QPS | NoSafeIn QPS | QPS median | QPS mean±std | 正向 rep | p99 median | vector-request delta | byte delta |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| ESCI | 245.32 | 252.49 | -2.93% | -2.82%±0.33% | 0/5 | +2.43% | +38.34% | -1.77% |
| COCO | 360.97 | 362.78 | -0.46% | -0.55%±0.30% | 0/5 | +0.68% | 0.00% | +5.47% |
| MSMARCO | 203.43 | 206.30 | -1.26% | -1.35%±0.18% | 0/5 | +4.51% | +14.66% | +0.47% |
| VoxCeleb2 | 197.16 | 200.28 | -1.56% | -1.64%±0.28% | 0/5 | -6.94% | 0.00% | +12.50% |

绝对 QPS 和 delta 均为五次中位数。全部原始 QPS delta：

- ESCI：`[-3.0177,-3.0837,-2.8385,-2.9270,-2.2563]%`；
- COCO：`[-0.8603,-0.1534,-0.8563,-0.4644,-0.4310]%`；
- MSMARCO：`[-1.2634,-1.2228,-1.4658,-1.6009,-1.1903]%`；
- VoxCeleb2：`[-1.8785,-1.5552,-1.3513,-1.9931,-1.4248]%`。

这里的 vector request 包含 VEC_ONLY/VEC_SPAN 和 VEC_ALL。SafeIn eager prefetch
分别产生约 156.11/73.36/122.43/60.40 个 VEC_ALL 请求/query，并把最终 payload
请求从 100/100/100/199.38 降到 53.16/57.68/46.20/168.52；但提前读取的字节、
额外请求和竞争成本仍未转化为 QPS 收益。Vox 的 p99 改善不改变其 QPS 与字节门禁
均失败的结论。

## 最低 NoSafeIn 固定基线

| 数据集 | 最低 NoSafeIn QPS | SafeIn median delta | mean±std | 正向 rep |
|---|---:|---:|---:|---:|
| ESCI | 252.04 | -2.67% | -2.64%±0.23% | 0/5 |
| COCO | 361.52 | -0.15% | -0.29%±0.27% | 0/5 |
| MSMARCO | 205.72 | -1.12% | -1.11%±0.23% | 0/5 |
| VoxCeleb2 | 199.92 | -1.38% | -1.45%±0.16% | 0/5 |

最低固定基线下也没有任何正向 repetition。该补充口径不覆盖正式配对统计，也不
参与 planner winner selection。

## 正确性审计与论文含义

- 40 个 measured results 全部满足 `GE,rho=0,reuse=0,tail=0`；
- NoSafeIn 的 `avg_all_read_requests=0`，四个数据集的 span payload view/reuse/credit
  均为 0；
- SafeIn 的 VEC_ALL 请求均非零，但 span payload view/reuse/credit 仍为 0；
- 每个配对的 Recall、probed、reranked、unique-fetch coverage 完全一致；
- planner fallback 为 0，planned bytes/requests 与 issued VEC_ONLY 一致。

论文应据此将传统 eager SafeIn prefetch 设为关闭，或把它作为负结果解释为何最终
采用 late materialization。该表不能否定 SafeIn-aware span credit；后者是独立的
`SE-GE` 机制问题，应由同 combined layout 的 rho 对照回答。

原始结果：

- `/home/zcq/VDB/test/recordgate_span_safein_prefetch_noreuse_ablation_20260718`
- `/home/zcq/VDB/test/recordgate_span_safein_prefetch_noreuse_smoke_20260718`
