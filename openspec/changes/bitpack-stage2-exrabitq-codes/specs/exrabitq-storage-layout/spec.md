## ADDED Requirements

### Requirement: ExRaBitQ Stage2 magnitude SHALL support bit-packed persistence
对于新的 packed-magnitude cluster store version，ExRaBitQ Stage2 magnitude code MUST 按当前 RabitQ bit width 持久化，而不是按每维一个 byte 持久化。持久化表示在 unpack 后 MUST 保留与当前 `uint8_t` 表示相同的逐维整数 magnitude 值。

#### Scenario: Bits2 magnitude 每维使用两个持久化 bit
- **当** builder 以 packed-magnitude 格式写入 `bits=2` 的 ExRaBitQ Stage2 block
- **则** 每个 Stage2 magnitude value 必须占用两个持久化 bit
- **并且** unpack 后必须恢复 `[0, 3]` 范围内的值

#### Scenario: Bits4 magnitude 每维使用四个持久化 bit
- **当** builder 以 packed-magnitude 格式写入 `bits=4` 的 ExRaBitQ Stage2 block
- **则** 每个 Stage2 magnitude value 必须占用四个持久化 bit
- **并且** unpack 后必须恢复 `[0, 15]` 范围内的值

### Requirement: Packed Stage2 storage SHALL be version-distinguished from byte-magnitude layouts
系统 MUST 使用显式 cluster store version 或等价 versioned layout marker，将 packed Stage2 magnitude 与现有 byte-magnitude `v10`/`v11` layout 区分开。

#### Scenario: Reader 打开旧 byte-magnitude index
- **当** reader 打开 `v10` 或 `v11` ExRaBitQ index
- **则** 它必须通过现有 byte-magnitude path 解析 Stage2 magnitude
- **并且** 它不得将旧格式 bytes 重新解释为 bit-packed magnitude

#### Scenario: Reader 打开 packed-magnitude index
- **当** reader 打开 packed-magnitude ExRaBitQ index
- **则** 它必须通过 packed-magnitude path 解析 Stage2 magnitude
- **并且** 它必须暴露 unpack 所需的 active bit width

### Requirement: Packed Stage2 indexes SHALL require rebuild rather than in-place mutation
系统 MUST 将 packed Stage2 magnitude storage 视为需要 rebuild 的索引格式变更。已有 byte-magnitude index MUST 对旧格式 reader 继续有效，并且 SHOULD NOT 被原地修改。

#### Scenario: 用户升级已有 byte-magnitude index
- **当** 用户希望为已有数据集使用 packed Stage2 magnitude storage
- **则** 系统必须要求构建新的 index directory
- **并且** benchmark metadata 必须标识该索引采用 packed Stage2 magnitude format

#### Scenario: Bits3 不在本变更的 packed Stage2 范围内
- **当** builder 被要求以本变更的新 packed-magnitude 格式写入 `bits=3` 的 ExRaBitQ Stage2 block
- **则** 系统必须走旧 byte-magnitude 格式或返回明确的不支持错误
- **并且** 系统不得静默写出未验证的 `bits=3` packed Stage2 layout
