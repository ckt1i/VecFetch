# Appendix Gate 8.15 报告（本次核查）

## 结论

- 8.15.1：PASS  
- 8.15.2：PASS  
- 8.15.3：PASS

## 核查脚本

- `openspec/changes/formal-baseline-study-execution/scripts/check_appendix_gate_8_15.py`
- 执行命令：
  `python openspec/changes/formal-baseline-study-execution/scripts/check_appendix_gate_8_15.py`

## 核查输入路径

- 主实验汇总（flatstor）  
  - `/home/zcq/VDB/baselines/formal-study/outputs/summaries/main_experiment_topk10_summary.csv`
  - `/home/zcq/VDB/baselines/formal-study/outputs/summaries/main_experiment_topk50_summary.csv`
  - `/home/zcq/VDB/baselines/formal-study/outputs/summaries/main_experiment_topk100_summary.csv`
- 扩展后端输入  
  - `/home/zcq/VDB/baselines/formal-study/outputs/extended_backend/selected_operating_points.csv`
  - `/home/zcq/VDB/baselines/formal-study/outputs/summaries/extended_backend_summary.csv`

## 说明

- 8.15.1 采用“块级完整性”定义：`main_experiment_topk10/50/100_summary.csv` 中每个数据集在对应 top-k 块存在结果。
- 8.15.2 采用“点级完整性”定义：`selected_operating_points.csv` 去重后的每个 `(dataset, system, variant, selected_param, backend)` 点，在 `extended_backend_summary.csv` 中存在至少一条成功记录（脚本默认按 `success` 字段做成功过滤；无该字段时按全量）。  
- 8.15.3 依赖 8.15.1 与 8.15.2 均 pass。
