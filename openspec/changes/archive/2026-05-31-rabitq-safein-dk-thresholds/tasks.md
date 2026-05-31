## 1. 元数据与配置

- [x] 1.1 在索引 metadata / schema 中增加 SafeIn 专用 `d_k` 字段和来源字段。
- [x] 1.2 增加 legacy exact `d_k` 与 SafeIn `d_k` 的运行时访问接口，并支持旧索引回退。
- [x] 1.3 增加 SafeIn `d_k` 校准空间、percentile、采样数、搜索范围和 nprobe 的配置字段。
- [x] 1.4 确保 metadata 加载能够兼容没有 SafeIn `d_k` 字段的旧索引。

## 2. RabitQ SafeIn d_k 校准

- [x] 2.1 在 codes、centroids 和 rotation 可用之后，实现 multi-bit RabitQ Stage2 SafeIn `d_k` 校准。
- [x] 2.2 支持 full-database 校准范围。
- [x] 2.3 支持与 serving 一致 coarse cluster 语义的 nprobe-limited 校准范围。
- [x] 2.4 将校准得到的 SafeIn `d_k` 和来源信息持久化到索引 metadata。
- [x] 2.5 为 benchmark 增加 SafeIn `d_k` 的值、空间、percentile、采样数、范围和 nprobe 输出字段。

## 3. 运行时分类

- [x] 3.1 更新 `ConANN::ClassifyAdaptive` 或等价运行时接口，使其使用 SafeIn `d_k` 进行 SafeIn 分类。
- [x] 3.2 更新 SafeOut 阈值计算，使 heap-full 模式只使用 query-time estimated kth distance 加 margin。
- [x] 3.3 更新 heap-not-full 行为，使 SafeOut 不再使用 static `d_k` 回退。
- [x] 3.4 保持现有 `Classify` API 的源码兼容，并保留 legacy SafeIn 回退行为。
- [x] 3.5 更新 `ClusterProber` 和 `OverlapScheduler` 调用点，以传递并快照新的动态 SafeOut 阈值语义。

## 4. 诊断与实验

- [x] 4.1 扩展离线诊断，报告 SafeIn `d_k` 来源和 SafeOut 阈值模式。
- [ ] 4.2 增加 exact-L2 SafeIn `d_k` 与 RabitQ Stage2 SafeIn `d_k` 的 replay / benchmark 对照。
- [ ] 4.3 增加 COCO100k 的 full-search RabitQ SafeIn `d_k` 校准验证。
- [ ] 4.4 增加 MSMARCO 的 nprobe-limited RabitQ SafeIn `d_k` 校准验证。
- [ ] 4.5 报告 SafeIn 数量、false SafeIn、false SafeIn rate、SafeOut 数量、false SafeOut，以及最终 recall 影响检查。

## 5. 验证

- [x] 5.1 增加 `ClassifyAdaptive` 的单元测试，覆盖 SafeIn `d_k`、动态 SafeOut，以及 heap-not-full 时不触发 SafeOut 的行为。
- [ ] 5.2 增加旧索引缺少 SafeIn `d_k` 字段时的 metadata 加载测试。
- [x] 5.3 增加小索引在 full / nprobe 范围下的校准 smoke test。
- [x] 5.4 构建本次变更影响到的 benchmark target。
- [ ] 5.5 运行 COCO100k 和 MSMARCO 验证命令，并把 summary 归档到诊断输出目录。
