# Problem Anchor

- **Bottom-line problem**: 在 record-return 向量检索中，把不可避免的原始向量验证读取转化为受控、可复用的连续 co-fetch，在不改变候选 membership 与 exact verification 语义的前提下，减少小规模随机 I/O 请求；并据此形成一条可被实验直接支撑的论文创新主线。
- **Must-solve bottleneck**: 分离的 vector/payload 读取使系统在候选验证与结果物化阶段产生大量小读；现有 streaming greedy 虽能合并读取，却不保证在固定有序 run 和读放大约束下得到请求数最少、字节数次少的分组，也没有严格利用 span 已覆盖的、可能有用的 inline payload bytes。
- **Non-goals**: 本轮不重新设计 ANN、RaBitQ/SafeOut 或 SafeIn 分类器；不把固定顺序连续分段包装成 NP-hard 装箱；不设计跨查询、跨 tile 的全局布局优化；不声称单次大读在所有设备和负载上总是优于小读；不让 co-fetch 或 SafeIn 改变最终结果正确性。
- **Constraints**: 论文与补实验剩余时间不足两周；复用当前 `.clu` 地址元数据、packed/split layout、64 KiB tile、async/serial reader、现有数据集与结果；优先采用无需训练、可解释、确定性的算法；所有正文结论必须区分已验证的 GE/vector-only exact 与仍需因果消融确认的 SE/SafeIn-aware 扩展。
- **Success condition**: 主方法能被精确定义为一个固定有序 run 上的受约束分段问题；exact planner 与独立二次 oracle 一致并以可接受开销运行；NoSpan、greedy/exact、Combined/NoCombine 的结果分别支撑请求合并、算法最优性和格式—执行协同。SafeIn-aware 只有在 paired 端到端结果、credit/reuse 遥测和 NoCombine 零 credit 边界共同通过时才进入主文，否则降为可选扩展或附录。
