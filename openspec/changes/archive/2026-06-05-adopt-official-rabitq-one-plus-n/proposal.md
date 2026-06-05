## Why

The current RaBitQ `bits` setting is ambiguous: Stage1 uses a 1-bit sign path while Stage2 stores `bits` magnitude bits plus a separate packed sign. This is not the official RaBitQ `1+n` semantics and makes the reported bit-width, memory footprint, and paper claims difficult to compare fairly.

This change introduces an official-compatible RaBitQ `1+n` format so `total_bits=4` means `1-bit Stage1 + 3-bit ExData`, while preserving the current compact resident preload and pipeline-oriented serving architecture.

## What Changes

- Add a rebuild-required `cluster.clu` format version for official-compatible RaBitQ `1+n` storage.
- Replace Stage2 `magnitude + ex_sign_packed` persistence with sign-folded `ex_code` persistence following official RaBitQ semantics.
- Split RaBitQ configuration and metadata into explicit `total_bits` and `ex_bits = total_bits - 1` fields where query, build, calibration, and benchmark code need unambiguous bit semantics.
- Update Stage2 query scoring to combine the Stage1 intermediate term with ExData scoring, using the official-style formula rather than reloading a separate Stage2 sign stream.
- Add SIMD packing/unpacking and dot-product support for the required ExData bit widths, including `ex_bits=3` for `total_bits=4`.
- Keep old v10/v11/v12 readers available for compatibility, but require rebuild for the new official-compatible index format.
- Update benchmark metadata so results state the RaBitQ storage format, `total_bits`, `ex_bits`, and whether legacy signed-magnitude Stage2 or official sign-folded ExData was used.

## Capabilities

### New Capabilities
- None.

### Modified Capabilities
- `exrabitq-storage-layout`: add a new official-compatible `1+n` ExData storage layout and versioning contract.
- `ipexrabitq-compact-layout`: extend compact blocked Stage2 layout to store sign-folded ExData rather than `magnitude + sign`.
- `exrabitq-stage2-kernel`: add Stage2 scoring kernels that consume sign-folded ExData and combine with the Stage1 intermediate term.
- `ivf-rabitq-baseline-storage`: persist unambiguous `total_bits/ex_bits` metadata and distinguish legacy and official-compatible formats.
- `ivf-rabitq-baseline-query`: query official-compatible indexes using official `1+n` scoring before exact rerank.
- `rabitq-safein-dk-calibration`: calibrate and report thresholds using `total_bits` semantics and the official-compatible Stage2 estimator.

## Impact

- Affected code paths: RaBitQ encoder/configuration, cluster writer/reader, `ParsedCluster`/resident views, Stage1-to-Stage2 candidate metadata, Stage2 SIMD kernels, SafeIn/SafeOut classification, ConANN epsilon calibration, benchmark metadata, and migration/debug tools.
- Existing v10/v11/v12 indexes remain readable by new binaries where currently supported, but official-compatible `1+n` indexes require rebuild and will not be readable by older binaries.
- Main experiments should report both `total_bits` and `ex_bits`; old legacy results must be labeled as signed-magnitude Stage2 rather than official RaBitQ `1+n`.
