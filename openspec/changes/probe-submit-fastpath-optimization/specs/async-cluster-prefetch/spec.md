## ADDED Requirements

### Requirement: Async read preparation SHALL support cached fixed-file data reads
async reader path SHALL 支持在 data file 已注册时，使用缓存的 fixed-file index 来准备 registered-buffer data read。现有基于 fd 的 preparation APIs MUST 继续保留为 fallback。

#### Scenario: Cached fixed-file index is used for hot data reads
- **WHEN** data file descriptor 已注册，且 scheduler 发射一个使用 registered buffer 的 vector-only read
- **THEN** 系统 SHALL 能够使用缓存的 fixed-file index 来准备 SQE
- **AND** 该热点路径 MUST 避免每个请求都做 registered-fd 查找

#### Scenario: Fallback remains available when fixed index is unavailable
- **WHEN** data file 未注册或缓存的 fixed-file index 无效
- **THEN** 系统 SHALL 回退到现有基于 fd 的 prep-read 行为
- **AND** 读取正确性必须保持不变

### Requirement: Async vector-only fast path SHALL preserve io_uring completion semantics
vector-only fast path SHALL 保留现有的 io_uring completion 语义，包括 user-data 到 pending slot 的映射、in-flight 计数、fixed-buffer 释放，以及对 isolated 或 shared reader 模式的兼容性。

#### Scenario: Completion dispatch resolves fast-path vector slots
- **WHEN** 一个 vector-only fast-path read 完成
- **THEN** completion dispatch SHALL 将 user-data token 解析到正确的 pending slot
- **AND** read buffer cleanup path MUST 恰好释放一次 fixed buffer 或 pooled buffer

#### Scenario: Reader mode compatibility is preserved
- **WHEN** scheduler 使用 shared reader 或 isolated reader
- **THEN** vector-only fast-path reads SHALL 保留现有的 submit、poll、wait、final-drain 行为
- **AND** tail drain 或 final drain 中不得丢失任何 completion
