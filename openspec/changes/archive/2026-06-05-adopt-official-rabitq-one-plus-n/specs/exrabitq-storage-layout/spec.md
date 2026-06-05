## MODIFIED Requirements

### Requirement: ExRaBitQ Region2 SHALL support a packed-sign entry format
当 `.clu` 文件使用 legacy signed-magnitude ExRaBitQ storage version 时，Region2 中的每条 ExRaBitQ entry MUST 使用 packed-sign 格式，而不是逐维 `uint8` sign 格式。该 entry MUST 至少包含 `ex_code[dim bytes or packed magnitude bytes]`、`packed_sign[ceil(dim/8) bytes]` 和 `xipnorm[4 bytes]`，并保持与旧 signed-magnitude Stage2 数学语义等价。该 packed-sign requirement SHALL NOT apply to official `1+n` sign-folded ExData formats.

#### Scenario: Packed-sign entry is written for a legacy signed-magnitude cluster
- **WHEN** 构建器以 legacy signed-magnitude ExRaBitQ storage version 写出一个 `bits > 1` 的 cluster
- **THEN** 每条 ExRaBitQ entry 或 batch block 必须包含 Stage2 magnitude payload
- **AND** 必须包含 `ceil(dim/8)` 字节的 packed sign 或等价 blocked packed-sign payload
- **AND** 必须包含 signed-magnitude estimator 所需的 scalar factor

#### Scenario: Packed-sign entry preserves legacy sign semantics
- **WHEN** 读取器按 legacy signed-magnitude format 解析一条 ExRaBitQ entry 或 batch block
- **THEN** 第 `i` 维 sign bit 的语义必须与 legacy `ex_sign[i]` 等价
- **AND** Stage2 估计结果必须与 legacy signed-magnitude format 在相同输入下保持一致，允许微小浮点误差

### Requirement: `.clu` file version SHALL explicitly distinguish old and new ExRaBitQ layouts
系统 MUST 使用显式的 `.clu` 文件版本区分旧的 byte-sign ExRaBitQ 布局、legacy packed-sign/packed-magnitude 布局和 official `1+n` sign-folded ExData 布局。新 reader MUST 按文件版本选择正确的 Region2 entry size、factor semantics 与 parse path，而不是依赖启发式推断。

#### Scenario: New reader parses old-format ExRaBitQ by version
- **WHEN** 读取器打开旧版本 `.clu` 文件
- **THEN** 它必须按旧的 `ex_code + ex_sign + xipnorm` 布局解析 Region2

#### Scenario: New reader parses packed-sign ExRaBitQ by version
- **WHEN** 读取器打开 legacy packed-sign 或 packed-magnitude `.clu` 文件
- **THEN** 它必须按 `magnitude + packed_sign + xipnorm` 或等价 blocked layout 解析 Region2
- **AND** 不得把版本判断委托给启发式 entry-size 猜测

#### Scenario: New reader parses official 1+n ExData by version
- **WHEN** 读取器打开 official `1+n` `.clu` 文件
- **THEN** 它必须按 sign-folded ExData layout 解析 Region2
- **AND** 它不得要求 Region2 包含独立 Stage2 sign payload

#### Scenario: Old reader does not silently accept new-format clusters
- **WHEN** 不支持 official `1+n` format 的旧 reader 打开 official `1+n` `.clu` 文件
- **THEN** 它必须以明确的不支持格式错误失败
- **AND** 不得静默按 legacy signed-magnitude layout 误解析

## ADDED Requirements

### Requirement: ExRaBitQ Region2 SHALL support official 1+n sign-folded ExData
系统 SHALL 支持一个 rebuild-required official `1+n` ExRaBitQ storage format。该格式 MUST 使用 `total_bits = 1 + ex_bits`，Stage2 Region2 MUST 持久化 sign-folded `ex_code` 和官方 estimator 所需 factor，而不得持久化独立 `ex_sign_packed`。

#### Scenario: Official ExData is written without a separate sign stream
- **WHEN** 构建器以 official `1+n` format 写出 `total_bits > 1` 的 cluster
- **THEN** Region2 必须包含按 `ex_bits = total_bits - 1` bit-pack 的 sign-folded `ex_code`
- **AND** Region2 必须包含 official Stage2 estimator 所需 factor
- **AND** Region2 不得包含独立的 `ex_sign_packed` payload

#### Scenario: Negative residual dimensions are sign-folded into ExData
- **WHEN** 编码器量化一个 residual 维度且该维度为负
- **THEN** 编码器必须按 official RaBitQ 语义对该维度的 ExData code 做 complement
- **AND** 查询路径必须能仅通过 ExData code 与 Stage1 intermediate term 还原 official Stage2 estimate

#### Scenario: Total-bits-one uses only Stage1 data
- **WHEN** `total_bits == 1`
- **THEN** `ex_bits` 必须为 0
- **AND** Region2 official ExData payload 必须为空或被标记为 absent
- **AND** 查询必须只使用 Stage1 estimator

### Requirement: Official 1+n metadata SHALL be explicit and observable
official `1+n` 索引 metadata SHALL 显式记录 bit semantics 与 estimator mode。runtime、benchmark 和 diagnostic 输出 MUST 能区分 legacy signed-magnitude format 与 official `1+n` format。

#### Scenario: New index metadata records official bit semantics
- **WHEN** 构建器写出 official `1+n` 索引
- **THEN** metadata 必须包含 `rabitq_total_bits`
- **AND** metadata 必须包含 `rabitq_ex_bits`
- **AND** metadata 必须包含 `rabitq_estimator_mode=official_1_plus_n`
- **AND** metadata 必须包含 `.clu` storage version

#### Scenario: Legacy index metadata is not mislabeled
- **WHEN** runtime 打开 legacy signed-magnitude 索引
- **THEN** metadata 和 benchmark 输出必须将其标记为 legacy signed-magnitude
- **AND** 不得把 legacy `bits=4` 报告为 official `total_bits=4`

### Requirement: Official 1+n storage SHALL define rebuild and rollback boundaries
official `1+n` format SHALL require rebuilding affected RaBitQ indexes. 新二进制 SHOULD retain supported legacy readers for comparison and rollback, but older binaries MUST NOT be assumed to read official `1+n` indexes.

#### Scenario: Official format requires rebuild
- **WHEN** 用户从 legacy signed-magnitude format 升级到 official `1+n` format
- **THEN** 系统必须要求重新构建 `.clu` index
- **AND** 不得原地重写旧索引目录

#### Scenario: Rollback uses legacy indexes
- **WHEN** serving 代码回滚到不支持 official `1+n` 的版本
- **THEN** 系统必须使用 legacy format 索引或重新 build
- **AND** 不得承诺旧 binary 可读取 official `1+n` `.clu`
