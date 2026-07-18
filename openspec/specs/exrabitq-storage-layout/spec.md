## ADDED Requirements

### Requirement: ExRaBitQ Region2 SHALL support a packed-sign entry format
当 `.clu` 文件使用新的 ExRaBitQ storage 版本时，Region2 中的每条 ExRaBitQ entry MUST 使用 packed-sign 格式，而不是逐维 `uint8` sign 格式。该 entry MUST 至少包含 `ex_code[dim bytes]`、`packed_sign[ceil(dim/8) bytes]` 和 `xipnorm[4 bytes]`，并保持与旧格式等价的数学语义。

#### Scenario: Packed-sign entry is written for a new-format cluster
- **WHEN** 构建器以新的 ExRaBitQ storage 版本写出一个 `bits > 1` 的 cluster
- **THEN** 每条 ExRaBitQ entry 必须包含 `dim` 字节的 `ex_code`
- **AND** 必须包含 `ceil(dim/8)` 字节的 packed sign
- **AND** 必须包含 4 字节的 `xipnorm`

#### Scenario: Packed-sign entry preserves sign semantics
- **WHEN** 读取器按新格式解析一条 ExRaBitQ entry
- **THEN** 第 `i` 维 sign bit 的语义必须与旧格式 `ex_sign[i]` 等价
- **AND** Stage2 估计结果必须与旧格式在相同输入下保持一致，允许微小浮点误差

### Requirement: `.clu` file version SHALL explicitly distinguish old and new ExRaBitQ layouts
系统 MUST 使用显式的 `.clu` 文件版本区分旧的 byte-sign ExRaBitQ 布局和新的 packed-sign ExRaBitQ 布局。新 reader MUST 按文件版本选择正确的 Region2 entry size 与 parse path，而不是依赖启发式推断。

#### Scenario: New reader parses old-format ExRaBitQ by version
- **WHEN** 读取器打开旧版本 `.clu` 文件
- **THEN** 它必须按旧的 `ex_code + ex_sign + xipnorm` 布局解析 Region2

#### Scenario: New reader parses packed-sign ExRaBitQ by version
- **WHEN** 读取器打开新版本 `.clu` 文件
- **THEN** 它必须按 `ex_code + packed_sign + xipnorm` 布局解析 Region2
- **AND** 不得把版本判断委托给启发式 entry-size 猜测

#### Scenario: Old reader does not silently accept new-format clusters
- **WHEN** 不支持新格式的旧 reader 打开 packed-sign `.clu` 文件
- **THEN** 它必须以明确的不支持格式错误失败
- **AND** 不得静默按旧布局误解析

### Requirement: Encoding output SHALL treat packed sign as the persisted ExRaBitQ sign representation
对于 `bits > 1` 的编码结果，系统 MUST 将 packed sign 视为 ExRaBitQ sign 的持久化主表示。若实现阶段保留逐维 sign 作为离线临时态或 debug 对拍手段，该 byte-sign 表示 MUST NOT 作为 cluster store 的主写入格式。

#### Scenario: Writer persists packed sign instead of byte-sign
- **WHEN** 编码器产出一个 `bits > 1` 的 RaBitQ code 并交给 cluster writer
- **THEN** writer 必须将 packed sign 写入 `.clu`
- **AND** 不得再把逐维 `ex_sign[dim bytes]` 写为新格式主表示

### Requirement: Parsed cluster views SHALL expose packed sign without re-materializing byte-sign buffers
读取器、resident preload 和 `ParsedCluster` 视图 MUST 直接向查询路径暴露 packed sign 访问方式，而不是在解析时把 packed sign 重新 materialize 成逐维 byte-sign buffer 作为 resident 主表示。

#### Scenario: Resident parsed cluster keeps packed-sign access
- **WHEN** resident preload 完成并生成 `ParsedCluster` 或等价 resident 视图
- **THEN** 该视图必须能够直接提供 packed-sign 指针或等价访问接口
- **AND** 不得要求额外常驻一个 `dim bytes` 的逐维 sign 展开缓冲

#### Scenario: Async parse path keeps packed-sign access
- **WHEN** query 通过解析 cluster block 生成 `ParsedCluster`
- **THEN** 解析结果必须保留 packed-sign 的零拷贝或等价轻量访问
- **AND** 不得因为兼容旧 kernel 而把 packed sign 展开成新的一段 byte-sign 所有权缓冲

### Requirement: Storage upgrade SHALL define rebuild and rollback boundaries
ExRaBitQ storage 格式升级 MUST 明确 rebuild、兼容和回滚边界。新格式索引可以要求重新构建；若 serving 二进制回退到不支持 packed-sign 的旧版本，系统 MUST 要求使用旧格式索引或重新 build，而不是承诺跨版本双向兼容。

#### Scenario: New-format index may require rebuild
- **WHEN** 用户希望从旧格式 ExRaBitQ 升级到 packed-sign 新格式
- **THEN** 系统可以要求重新构建 `.clu` 索引
- **AND** 该要求必须在变更说明或 benchmark 元数据中清晰表达

#### Scenario: Rollback requires matching reader and index format
- **WHEN** serving 代码回退到不支持 packed-sign 的旧 reader
- **THEN** 系统必须要求使用旧格式索引或重新 build
- **AND** 不得承诺旧二进制可直接读取新格式 `.clu`
## Requirements
### Requirement: ExRaBitQ cleanup SHALL preserve frozen and baseline layouts
The cleanup SHALL preserve support for the frozen method layout and the official-like baseline or compatibility layout. In particular, `tile_lane_bitmajor` and `vector_bitplanes` SHALL remain usable for reading, benchmarking, and validation unless a later migration change explicitly removes compatibility.

#### Scenario: Frozen method layout remains readable
- **WHEN** a query loads an existing index written with the frozen method layout
- **THEN** the reader MUST parse the layout successfully
- **AND** the query path MUST execute with the same fixed active ex-bits semantics as before cleanup

#### Scenario: Official-like baseline layout remains available
- **WHEN** a benchmark compares against the official-like RabitQ baseline layout
- **THEN** the corresponding layout MUST remain available
- **AND** cleanup MUST NOT silently remap it to the method layout

### Requirement: ExRaBitQ cleanup SHALL use conservative SIMD removal
This cleanup SHALL NOT delete low-level SIMD helper or kernel implementations solely because abandoned upper-level features no longer call them. SIMD deletion MAY occur only in a later change after proving that the helper is unused by frozen, baseline, compatibility, and test paths.

#### Scenario: Upper-level progressive calls are removed but SIMD helpers remain
- **WHEN** progressive pruning is removed from the query pipeline
- **THEN** upper-level calls from serving and benchmark paths MUST be removed
- **AND** shared SIMD helpers MAY remain compiled and tested

### Requirement: ExRaBitQ cleanup SHALL not require index rebuild
The cleanup SHALL be an implementation and API-surface cleanup, not a storage migration. Existing indexes using the frozen or baseline layouts MUST NOT require rebuild only because abandoned query features were removed.

#### Scenario: Existing frozen-format index is reused after cleanup
- **WHEN** validation reruns on a previously built frozen-format index
- **THEN** the query must load the index without rebuild
- **AND** recall and speed must be compared under the same physical index files

