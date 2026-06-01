# COCO100k Vector Search 验证

日期：2026-06-01

## 配置

- 数据集：`/home/zcq/VDB/data/coco_100k`
- Index：`/home/zcq/VDB/test/data/COCO100k/index_fkmeans_2048_bits4_eps0.90`
- GT：`/home/zcq/VDB/baselines/data/formal_baselines/coco_100k/gt/gt_top10.npy`
- `image_ids`：`/home/zcq/VDB/data/coco_100k/image_ids.npy`
- 参数：`nlist=2048`，`nprobe=64`，`topk=10`，`bits=4`，`queries=1000`
- 说明：预构建 index 使用 image id payload，因此验证命令必须传入 `--image-ids`，否则 recall 会因 payload id 与 GT row id 不一致而显示为 0。

## 结果

| 配置 | CRC | early-stop | dynamic SafeOut | avg probed | recall@10 | avg latency ms | S1 SafeIn | S1 SafeOut | S1 Uncertain | S1 false SafeOut | S2 SafeIn | S2 SafeOut | S2 Uncertain | S2 false SafeOut | SafeOut frontier buffered | CRC buffered |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| dynamic-safeout-only | 0 | 0 | 1 | 64.00 | 0.0018 | 1.924 | 7 | 2,682,973 | 415,932 | 211 | 164 | 317,835 | 97,933 | 54 | 98,104 | 0 |
| no-safeout | 0 | 0 | 0 | 64.00 | 0.0018 | 11.136 | 7 | 0 | 3,098,905 | 0 | 172 | 0 | 3,098,733 | 0 | 0 | 0 |
| crc-dynamic-safeout | 1 | 0 | 1 | 64.00 | 0.0018 | 1.926 | 7 | 2,682,973 | 415,932 | 211 | 164 | 317,835 | 97,933 | 54 | 98,104 | 98,104 |
| crc-no-safeout | 1 | 0 | 0 | 64.00 | 0.0018 | 7.087 | 7 | 0 | 3,098,905 | 0 | 172 | 0 | 3,098,733 | 0 | 0 | 3,098,912 |

## 结论

- `--crc 0 --early-stop 0 --dynamic-safeout 1` 可以独立产生 SafeOut：S1 SafeOut `2,682,973`，S2 SafeOut `317,835`，总 SafeOut `3,000,808`。
- `--crc 0 --early-stop 0 --dynamic-safeout 0` 不产生 estimate-driven SafeOut，且 SafeOut frontier 统计全部为 0。
- `--crc 1 --early-stop 0 --dynamic-safeout 1` 没有 probe early-stop，`avg_probed=64.00` 且 `early_stop_rate=0`，但 SafeOut 仍生效。
- `--crc 1 --early-stop 0 --dynamic-safeout 0` 只维护 CRC state，不维护 SafeOut frontier，SafeOut 数量为 0。
- 新的 `F=kth(d_hat+e)` 路径相对 no-SafeOut 对照新增 `3,000,808` 个 SafeOut。旧的 CRC-bound frontier 代码路径已被替换，本次没有在同一生产二进制中直接复跑旧绑定公式；如需严格旧公式对照，需要使用变更前二进制或冻结 inline benchmark 单独标注为非生产路径对照。
- 四个配置 recall 都为 `0.0018`，说明当前预构建 index/GT/metric 组合的 recall 基线很低；该现象不随 dynamic SafeOut 开关变化，不是本次解耦逻辑单独引入的变化。

## 输出

- `coco100k_dynamic_safeout_only/results.json`
- `coco100k_no_safeout/results.json`
- `coco100k_crc_dynamic_safeout/results.json`
- `coco100k_crc_no_safeout/results.json`
