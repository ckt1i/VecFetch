#include "vdb/storage/cluster_store.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <fstream>
#include <limits>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "vdb/simd/ip_exrabitq.h"
#include "vdb/simd/popcount.h"
#include "vdb/storage/pack_codes.h"

#ifndef MADV_COLLAPSE
#define MADV_COLLAPSE 25
#endif

namespace vdb {
namespace storage {

static constexpr uint32_t kGlobalMagic = 0x4C4D4356;
static constexpr uint32_t kFileVersion = 12;
static constexpr uint32_t kFileVersionV15 = 15;
static constexpr uint32_t kFileVersionV14 = 14;
static constexpr uint32_t kFileVersionV13 = 13;
static constexpr uint32_t kFileVersionV11 = 11;
static constexpr uint32_t kFileVersionV10 = 10;
static constexpr uint32_t kFileVersionV9 = 9;
static constexpr uint32_t kFileVersionV8 = 8;
static constexpr uint32_t kFileVersionV7 = 7;
static constexpr uint32_t kAlignSize = 4096;
static constexpr uint32_t kBlockMagic = 0x424C4356;
static constexpr uint32_t kAddressFormatV2 = 2;
static constexpr uint32_t kVariableExRegionMagic = 0x315A5845;  // "EXZ1"

namespace {

inline uint32_t PackedSignBytes(Dim dim) {
    return (dim + 7) / 8;
}

inline uint32_t ExRaBitQBatchSize() {
    return 8;
}

inline uint32_t ExRaBitQDimBlock() {
    return 64;
}

inline bool SupportsPackedStage2MagnitudeBits(uint8_t bits) {
    return bits == 2 || bits == 4;
}

inline bool SupportsOfficialExDataBits(uint8_t ex_bits) {
    return ex_bits == 0 || ex_bits == 1 || ex_bits == 2 || ex_bits == 3 || ex_bits == 4;
}

inline bool UsesVariableOfficialExDataLayout(RaBitQExDataLayout layout) {
    const RaBitQExDataLayout resolved = RaBitQResolveSelectedExDataLayout(layout);
    return resolved == RaBitQExDataLayout::kSplit3TrimmedBitplanes ||
           resolved == RaBitQExDataLayout::kSplit3ZeroPlaneElide ||
           resolved == RaBitQExDataLayout::kVectorBitplanes ||
           resolved == RaBitQExDataLayout::kVectorBitplanesPrefetch ||
           resolved == RaBitQExDataLayout::kVectorBitplanesMicroBatch ||
           resolved == RaBitQExDataLayout::kVectorBitMajorTiles ||
           resolved == RaBitQExDataLayout::kVectorNibble4 ||
           resolved == RaBitQExDataLayout::kVector2Bit ||
           resolved == RaBitQExDataLayout::kSmallLane4Bitplanes ||
           resolved == RaBitQExDataLayout::kSmallLane2Bitplanes;
}

inline bool IsVectorBitplanesLayout(RaBitQExDataLayout layout) {
    const RaBitQExDataLayout resolved = RaBitQResolveSelectedExDataLayout(layout);
    return resolved == RaBitQExDataLayout::kVectorBitplanes ||
           resolved == RaBitQExDataLayout::kVectorBitplanesPrefetch ||
           resolved == RaBitQExDataLayout::kVectorBitplanesMicroBatch;
}

inline bool IsVectorBitMajorTileLayout(RaBitQExDataLayout layout) {
    const RaBitQExDataLayout resolved = RaBitQResolveSelectedExDataLayout(layout);
    return resolved == RaBitQExDataLayout::kVectorBitMajorTiles;
}

inline bool IsVectorContiguousOfficialLayout(RaBitQExDataLayout layout) {
    const RaBitQExDataLayout resolved = RaBitQResolveSelectedExDataLayout(layout);
    return IsVectorBitplanesLayout(resolved) ||
           IsVectorBitMajorTileLayout(resolved) ||
           resolved == RaBitQExDataLayout::kVectorNibble4 ||
           resolved == RaBitQExDataLayout::kVector2Bit;
}

inline bool IsSmallLaneBitplanesLayout(RaBitQExDataLayout layout) {
    const RaBitQExDataLayout resolved = RaBitQResolveSelectedExDataLayout(layout);
    return resolved == RaBitQExDataLayout::kSmallLane4Bitplanes ||
           resolved == RaBitQExDataLayout::kSmallLane2Bitplanes;
}

inline uint32_t SmallLaneBitplanesGroupSize(RaBitQExDataLayout layout) {
    const RaBitQExDataLayout resolved = RaBitQResolveSelectedExDataLayout(layout);
    if (resolved == RaBitQExDataLayout::kSmallLane2Bitplanes) {
        return 2;
    }
    return 4;
}

inline uint32_t EffectiveLegacyWriterFileVersion(uint8_t bits) {
    const char* env = std::getenv("VDB_CLUSTER_STORE_VERSION");
    if (env == nullptr || env[0] == '\0') {
        return (bits > 1 && !SupportsPackedStage2MagnitudeBits(bits))
            ? kFileVersionV11
            : kFileVersion;
    }
    const long ver = std::strtol(env, nullptr, 10);
    if (ver == static_cast<long>(kFileVersionV10)) return kFileVersionV10;
    if (ver == static_cast<long>(kFileVersionV11)) return kFileVersionV11;
    if (ver == static_cast<long>(kFileVersion)) return kFileVersion;
    return (bits > 1 && !SupportsPackedStage2MagnitudeBits(bits))
        ? kFileVersionV11
        : kFileVersion;
}

inline uint32_t EffectiveWriterFileVersion(const RaBitQConfig& cfg) {
    if (cfg.uses_official_1_plus_n()) {
        if (UsesVariableOfficialExDataLayout(cfg.effective_exdata_layout())) {
            return kFileVersionV15;
        }
        return RaBitQExDataLayoutIsDirect(cfg.effective_exdata_layout())
            ? kFileVersionV14
            : kFileVersionV13;
    }
    return EffectiveLegacyWriterFileVersion(cfg.bits);
}

inline void WriteRaw(std::fstream& f, const void* data, size_t len) {
    f.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
}

template <typename T>
inline void WriteVal(std::fstream& f, T value) {
    WriteRaw(f, &value, sizeof(T));
}

template <typename T>
inline bool PreadValue(int fd, off_t offset, T& out) {
    ssize_t n = ::pread(fd, &out, sizeof(T), offset);
    return n == static_cast<ssize_t>(sizeof(T));
}

template <typename T>
inline void AppendVal(std::vector<uint8_t>& out, T value) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

inline void AppendRaw(std::vector<uint8_t>& out, const void* data, size_t len) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    out.insert(out.end(), p, p + len);
}

inline uint32_t PopcountLow8(uint32_t v) {
    return static_cast<uint32_t>(__builtin_popcount(v & 0xFFu));
}

inline bool PreadBytes(int fd, off_t offset, void* out, size_t len) {
    constexpr size_t kMaxChunk = 1u << 30;  // 1 GiB per syscall, stay below kernel short-read limits.
    uint8_t* dst = static_cast<uint8_t*>(out);
    size_t remaining = len;
    off_t cur_off = offset;
    while (remaining > 0) {
        const size_t chunk = std::min(remaining, kMaxChunk);
        ssize_t n = ::pread(fd, dst, chunk, cur_off);
        if (n != static_cast<ssize_t>(chunk)) {
            return false;
        }
        dst += chunk;
        cur_off += static_cast<off_t>(chunk);
        remaining -= chunk;
    }
    return true;
}

inline uint64_t RoundUp4K(uint64_t v) {
    return (v + kAlignSize - 1) & ~(static_cast<uint64_t>(kAlignSize) - 1);
}

inline void PadTo4K(std::fstream& f, uint64_t& offset) {
    uint64_t aligned = RoundUp4K(offset);
    if (aligned > offset) {
        uint64_t pad = aligned - offset;
        char zeros[kAlignSize] = {0};
        while (pad > 0) {
            uint64_t chunk = std::min(pad, static_cast<uint64_t>(kAlignSize));
            f.write(zeros, static_cast<std::streamsize>(chunk));
            pad -= chunk;
        }
        offset = aligned;
    }
}

void BuildResidentParallelStage2View(const query::ParsedCluster& parsed,
                                     ClusterStoreReader::ResidentClusterView* view) {
    if (view == nullptr) return;
    if (parsed.exrabitq_storage_version < kFileVersionV11 ||
        parsed.uses_official_1_plus_n() ||
        parsed.exrabitq_batch_blocks == nullptr ||
        parsed.exrabitq_magnitude_packed ||
        parsed.exrabitq_batch_size == 0 ||
        parsed.exrabitq_dim_block == 0 ||
        parsed.exrabitq_num_dim_blocks == 0 ||
        parsed.exrabitq_num_batch_blocks == 0) {
        return;
    }

    const uint32_t batch_size = parsed.exrabitq_batch_size;
    const uint32_t dim_block = parsed.exrabitq_dim_block;
    const uint32_t num_dim_blocks = parsed.exrabitq_num_dim_blocks;
    const uint32_t num_batch_blocks = parsed.exrabitq_num_batch_blocks;
    const uint32_t slices_per_dim_block = dim_block / 16;
    const uint32_t sign_block_bytes = dim_block / 8;
    const uint32_t abs_block_size = num_dim_blocks * slices_per_dim_block * batch_size * 16;
    const uint32_t sign_words_per_block =
        num_dim_blocks * slices_per_dim_block * batch_size;

    view->exrabitq_parallel_abs_block_size = abs_block_size;
    view->exrabitq_parallel_sign_words_per_block = sign_words_per_block;
    view->exrabitq_parallel_slices_per_dim_block = slices_per_dim_block;
    view->exrabitq_parallel_abs_blocks_storage.resize(
        static_cast<size_t>(num_batch_blocks) * abs_block_size);
    view->exrabitq_parallel_sign_words_storage.resize(
        static_cast<size_t>(num_batch_blocks) * sign_words_per_block);

    for (uint32_t bb = 0; bb < num_batch_blocks; ++bb) {
        const auto block_view = parsed.exrabitq_batch_block_view(bb);
        uint8_t* abs_out =
            view->exrabitq_parallel_abs_blocks_storage.data() +
            static_cast<size_t>(bb) * abs_block_size;
        uint16_t* sign_out =
            view->exrabitq_parallel_sign_words_storage.data() +
            static_cast<size_t>(bb) * sign_words_per_block;
        for (uint32_t db = 0; db < num_dim_blocks; ++db) {
            const uint8_t* abs_src_block =
                block_view.abs_blocks + static_cast<size_t>(db) * batch_size * dim_block;
            const uint8_t* sign_src_block =
                block_view.sign_blocks + static_cast<size_t>(db) * batch_size * sign_block_bytes;
            for (uint32_t sub = 0; sub < slices_per_dim_block; ++sub) {
                for (uint32_t lane = 0; lane < batch_size; ++lane) {
                    const size_t abs_idx =
                        (((static_cast<size_t>(db) * slices_per_dim_block + sub) * batch_size) +
                         lane) * 16;
                    std::memcpy(abs_out + abs_idx,
                                abs_src_block + static_cast<size_t>(lane) * dim_block + sub * 16,
                                16);
                    uint16_t sign_word = 0;
                    std::memcpy(&sign_word,
                                sign_src_block + static_cast<size_t>(lane) * sign_block_bytes +
                                    sub * sizeof(uint16_t),
                                sizeof(uint16_t));
                    sign_out[(static_cast<size_t>(db) * slices_per_dim_block + sub) *
                                 batch_size +
                             lane] = sign_word;
                }
            }
        }
    }
}

bool UseCompactResidentPreload() {
	    const char* mode = std::getenv("VDB_RESIDENT_PRELOAD_MODE");
	    if (mode != nullptr && mode[0] != '\0') {
	        if (std::strcmp(mode, "full_file") == 0 ||
	            std::strcmp(mode, "full_file_mmap_2mb") == 0 ||
	            std::strcmp(mode, "legacy_full_file") == 0 ||
	            std::strcmp(mode, "legacy") == 0) {
	            return false;
	        }
        return true;
    }
    const char* compact = std::getenv("VDB_COMPACT_RESIDENT_PRELOAD");
    if (compact != nullptr &&
        (std::strcmp(compact, "0") == 0 ||
         std::strcmp(compact, "false") == 0 ||
         std::strcmp(compact, "off") == 0)) {
        return false;
    }
    return true;
}

bool UseResidentHugePages() {
    const char* env = std::getenv("VDB_RESIDENT_HUGEPAGE");
    if (env == nullptr || env[0] == '\0') {
        env = std::getenv("VDB_MADVISE_HUGEPAGE");
    }
    return env != nullptr && env[0] != '\0' && env[0] != '0' &&
           std::strcmp(env, "false") != 0 && std::strcmp(env, "off") != 0;
}

bool UseResidentHugePageCollapse() {
    const char* env = std::getenv("VDB_RESIDENT_HUGEPAGE_COLLAPSE");
    return env != nullptr && env[0] != '\0' && env[0] != '0' &&
           std::strcmp(env, "false") != 0 && std::strcmp(env, "off") != 0;
}

bool UseAlignedMmapResidentFile() {
    const char* mode = std::getenv("VDB_RESIDENT_PRELOAD_MODE");
    if (mode != nullptr && std::strcmp(mode, "full_file_mmap_2mb") == 0) {
        return true;
    }
    const char* alloc = std::getenv("VDB_RESIDENT_FILE_ALLOC");
    if (alloc != nullptr &&
        (std::strcmp(alloc, "mmap_2mb") == 0 ||
         std::strcmp(alloc, "aligned_mmap") == 0 ||
         std::strcmp(alloc, "mmap") == 0)) {
        return true;
    }
    const char* env = std::getenv("VDB_RESIDENT_MMAP_2MB");
    return env != nullptr && env[0] != '\0' && env[0] != '0' &&
           std::strcmp(env, "false") != 0 && std::strcmp(env, "off") != 0;
}

bool UseCompactCodeSlabResidentPreload() {
    const char* mode = std::getenv("VDB_RESIDENT_PRELOAD_MODE");
    if (mode != nullptr &&
        (std::strcmp(mode, "compact_code_mmap_2mb") == 0 ||
         std::strcmp(mode, "compact_code_slab_mmap_2mb") == 0)) {
        return true;
    }
    const char* env = std::getenv("VDB_RESIDENT_CODE_SLAB_MMAP_2MB");
    return env != nullptr && env[0] != '\0' && env[0] != '0' &&
           std::strcmp(env, "false") != 0 && std::strcmp(env, "off") != 0;
}

bool UseStage1EnvelopePrecompute() {
    const char* env = std::getenv("VDB_STAGE1_PRECOMPUTE_ENVELOPE");
    return env != nullptr && env[0] != '\0' && env[0] != '0' &&
           std::strcmp(env, "false") != 0 && std::strcmp(env, "off") != 0;
}

uintptr_t AlignUp(uintptr_t value, uintptr_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

uint64_t AlignUp64(uint64_t value, uint64_t alignment) {
    return (value + alignment - 1u) & ~(alignment - 1u);
}

void MAdviseHugePageRange(void* ptr, size_t len) {
    if (!UseResidentHugePages() || ptr == nullptr || len < (2u << 20)) {
        return;
    }
    const long page_size_long = ::sysconf(_SC_PAGESIZE);
    const uintptr_t page_size =
        page_size_long > 0 ? static_cast<uintptr_t>(page_size_long) : 4096u;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(ptr);
    const uintptr_t end = begin + len;
    const uintptr_t aligned_begin = begin & ~(page_size - 1u);
    const uintptr_t aligned_end = (end + page_size - 1u) & ~(page_size - 1u);
    if (aligned_end <= aligned_begin) {
        return;
    }
    (void)::madvise(reinterpret_cast<void*>(aligned_begin),
                    static_cast<size_t>(aligned_end - aligned_begin),
                    MADV_HUGEPAGE);
    if (UseResidentHugePageCollapse()) {
        (void)::madvise(reinterpret_cast<void*>(aligned_begin),
                        static_cast<size_t>(aligned_end - aligned_begin),
                        MADV_COLLAPSE);
    }
}

template <typename T>
void MAdviseHugePageVector(std::vector<T>& vec) {
    if (!vec.empty()) {
        MAdviseHugePageRange(vec.data(), vec.size() * sizeof(T));
    }
}

constexpr uint8_t kFastScanInvPerm[16] = {
    0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15
};

VDB_FORCE_INLINE uint16_t FastScanPresenceMaskFull(const uint8_t* group_bytes) {
    uint16_t mask = 0;
    for (uint32_t i = 0; i < 16; ++i) {
        const uint8_t byte = group_bytes[i];
        mask |= static_cast<uint16_t>(1u << (byte & 0x0Fu));
        mask |= static_cast<uint16_t>(1u << ((byte >> 4) & 0x0Fu));
    }
    return mask;
}

VDB_FORCE_INLINE uint16_t FastScanPresenceMaskTail(const uint8_t* group_bytes,
                                                   uint32_t count) {
    uint16_t mask = 0;
    for (uint32_t lane = 0; lane < count; ++lane) {
        const bool high_half = lane >= 16u;
        const uint32_t lane_in_half = high_half ? (lane - 16u) : lane;
        const uint32_t perm_pos = kFastScanInvPerm[lane_in_half];
        const uint8_t byte = group_bytes[perm_pos];
        const uint8_t nibble =
            high_half ? static_cast<uint8_t>((byte >> 4) & 0x0Fu)
                      : static_cast<uint8_t>(byte & 0x0Fu);
        mask |= static_cast<uint16_t>(1u << nibble);
    }
    return mask;
}

VDB_FORCE_INLINE uint16_t FastScanPresenceMaskForGroup(const uint8_t* packed_codes,
                                                       uint32_t group_idx,
                                                       uint32_t count) {
    const uint8_t* pair = packed_codes + (group_idx / 2u) * 32u;
    const uint8_t* group_bytes = pair + ((group_idx & 1u) ? 16u : 0u);
    return count >= 32u ? FastScanPresenceMaskFull(group_bytes)
                        : FastScanPresenceMaskTail(group_bytes, count);
}

uint64_t ResidentStage1EnvelopeBytes(
    const ClusterStoreReader::ResidentClusterView& view) {
    return static_cast<uint64_t>(view.stage1_envelope_presence_masks.size()) *
               sizeof(uint16_t) +
           static_cast<uint64_t>(view.stage1_envelope_norm_min.size()) *
               sizeof(float) +
           static_cast<uint64_t>(view.stage1_envelope_norm_max.size()) *
               sizeof(float);
}

void BuildResidentStage1EnvelopeSummary(
    const query::ParsedCluster& parsed,
    Dim dim,
    ClusterStoreReader::ResidentClusterView* view) {
    if (view == nullptr || parsed.fastscan_blocks == nullptr ||
        parsed.num_fastscan_blocks == 0 || parsed.fastscan_block_size == 0 ||
        dim < 4) {
        return;
    }
    const uint32_t groups = dim >> 2;
    const uint32_t packed_sz = FastScanPackedSize(dim);
    if (groups == 0 ||
        parsed.fastscan_block_size < packed_sz + 32u * sizeof(float)) {
        return;
    }

    view->stage1_envelope_groups_per_block = groups;
    view->stage1_envelope_presence_masks.assign(
        static_cast<size_t>(parsed.num_fastscan_blocks) * groups, 0);
    view->stage1_envelope_norm_min.assign(parsed.num_fastscan_blocks, 0.0f);
    view->stage1_envelope_norm_max.assign(parsed.num_fastscan_blocks, 0.0f);

    for (uint32_t b = 0; b < parsed.num_fastscan_blocks; ++b) {
        const uint32_t base_idx = b * 32u;
        const uint32_t count = std::min(32u, parsed.num_records - base_idx);
        const uint8_t* block_ptr =
            parsed.fastscan_blocks +
            static_cast<size_t>(b) * parsed.fastscan_block_size;
        for (uint32_t group = 0; group < groups; ++group) {
            view->stage1_envelope_presence_masks[
                static_cast<size_t>(b) * groups + group] =
                FastScanPresenceMaskForGroup(block_ptr, group, count);
        }
        const float* norms = reinterpret_cast<const float*>(block_ptr + packed_sz);
        float norm_min = std::numeric_limits<float>::infinity();
        float norm_max = -std::numeric_limits<float>::infinity();
        for (uint32_t i = 0; i < count; ++i) {
            norm_min = std::min(norm_min, norms[i]);
            norm_max = std::max(norm_max, norms[i]);
        }
        view->stage1_envelope_norm_min[b] = norm_min;
        view->stage1_envelope_norm_max[b] = norm_max;
    }
}

void CopyParsedMetadataToResident(const query::ParsedCluster& parsed,
                                  ClusterStoreReader::ResidentClusterView* view) {
    view->fastscan_block_size = parsed.fastscan_block_size;
    view->num_fastscan_blocks = parsed.num_fastscan_blocks;
    view->exrabitq_entry_size = parsed.exrabitq_entry_size;
    view->exrabitq_sign_bytes = parsed.exrabitq_sign_bytes;
    view->exrabitq_sign_packed = parsed.exrabitq_sign_packed;
    view->exrabitq_storage_version = parsed.exrabitq_storage_version;
    view->exrabitq_batch_block_size = parsed.exrabitq_batch_block_size;
    view->exrabitq_batch_block_offsets = parsed.exrabitq_batch_block_offsets;
    view->exrabitq_batch_region_bytes = parsed.exrabitq_batch_region_bytes;
    view->exrabitq_variable_batch_blocks = parsed.exrabitq_variable_batch_blocks;
    view->exrabitq_batch_size = parsed.exrabitq_batch_size;
    view->exrabitq_dim_block = parsed.exrabitq_dim_block;
    view->exrabitq_num_dim_blocks = parsed.exrabitq_num_dim_blocks;
    view->exrabitq_num_batch_blocks = parsed.exrabitq_num_batch_blocks;
    view->exrabitq_abs_bytes_per_lane_dim_block =
        parsed.exrabitq_abs_bytes_per_lane_dim_block;
    view->exrabitq_magnitude_bits = parsed.exrabitq_magnitude_bits;
    view->exrabitq_magnitude_packed = parsed.exrabitq_magnitude_packed;
    view->rabitq_total_bits = parsed.rabitq_total_bits;
    view->rabitq_ex_bits = parsed.rabitq_ex_bits;
    view->rabitq_estimator_mode = parsed.rabitq_estimator_mode;
    view->rabitq_exdata_layout = parsed.rabitq_exdata_layout;
    view->num_records = parsed.num_records;
    view->epsilon = parsed.epsilon;
    view->address_page_size = parsed.address_page_size;
}

void RelocateResidentCodePointersToBase(
    const query::ParsedCluster& parsed,
    const uint8_t* base,
    ClusterStoreReader::ResidentClusterView* view) {
    view->fastscan_blocks = base == nullptr ? nullptr
                                            : base + parsed.fastscan_region_offset;
    view->exrabitq_entries =
        (base != nullptr && parsed.exrabitq_entries != nullptr)
            ? base + parsed.exrabitq_region_offset
            : nullptr;
    view->exrabitq_batch_blocks =
        (base != nullptr && parsed.exrabitq_batch_blocks != nullptr)
            ? base + parsed.exrabitq_region_offset
            : nullptr;
    view->exrabitq_batch_block_offsets =
        (base != nullptr && parsed.exrabitq_batch_block_offsets != nullptr)
            ? reinterpret_cast<const uint32_t*>(base + parsed.exrabitq_region_offset + 8)
            : nullptr;
}

void RelocateResidentCodePointers(const query::ParsedCluster& parsed,
                                  ClusterStoreReader::ResidentClusterView* view) {
    const uint8_t* base = view->code_storage.empty() ? nullptr
                                                     : view->code_storage.data();
    RelocateResidentCodePointersToBase(parsed, base, view);
}

ClusterStoreReader::ResidentClusterView MakeCompactResidentView(
    query::ParsedCluster& parsed) {
    ClusterStoreReader::ResidentClusterView view;
    CopyParsedMetadataToResident(parsed, &view);
    if (parsed.code_region_bytes > 0) {
        view.code_storage.assign(parsed.fastscan_blocks,
                                 parsed.fastscan_blocks + parsed.code_region_bytes);
        MAdviseHugePageVector(view.code_storage);
    }
    view.code_storage_bytes = static_cast<uint64_t>(view.code_storage.size());
    RelocateResidentCodePointers(parsed, &view);
    view.raw_addresses = nullptr;
    view.addresses_are_raw_v2 = false;
    view.raw_address_bytes = 0;
    view.decoded_addresses = std::move(parsed.decoded_addresses);
    view.decoded_address_bytes =
        static_cast<uint64_t>(view.decoded_addresses.size()) *
        sizeof(AddressEntry);
    return view;
}

}  // namespace

ClusterStoreWriter::ClusterStoreWriter() = default;

ClusterStoreWriter::~ClusterStoreWriter() {
    if (file_.is_open() && !finalized_) {
        file_.close();
    }
}

uint64_t ClusterStoreWriter::lookup_entry_size() const {
    return 4 + 4 + 4 + static_cast<uint64_t>(info_.dim) * 4 + 8 + 8 + 4 + 4;
}

static constexpr uint32_t kMaxPathLen = 256;

Status ClusterStoreWriter::Open(const std::string& path,
                                uint32_t num_clusters,
                                Dim dim,
                                const RaBitQConfig& rabitq_config) {
    if (file_.is_open()) {
        return Status::InvalidArgument("ClusterStoreWriter already open");
    }

    path_ = path;
    info_.num_clusters = num_clusters;
    info_.dim = dim;
    info_.rabitq_config = rabitq_config;
    info_.rabitq_config.storage_version =
        static_cast<uint8_t>(EffectiveWriterFileVersion(info_.rabitq_config));
    if (info_.rabitq_config.uses_official_1_plus_n()) {
        if (!info_.rabitq_config.official_bits_valid()) {
            return Status::InvalidArgument(
                "official RaBitQ requires total_bits == ex_bits + 1");
        }
        if (!SupportsOfficialExDataBits(info_.rabitq_config.ex_bits)) {
            return Status::InvalidArgument(
                "official RaBitQ ExData supports ex_bits=0,1,2,3,4");
        }
        if (!info_.rabitq_config.exdata_layout_valid()) {
            return Status::InvalidArgument(
                "official optimized ExData layout is incompatible with ex_bits");
        }
        info_.rabitq_config.exdata_layout =
            info_.rabitq_config.effective_exdata_layout();
        info_.rabitq_config.bits =
            info_.rabitq_config.ex_bits > 0 ? info_.rabitq_config.ex_bits : 1;
    } else {
        if (!info_.rabitq_config.exdata_layout_valid()) {
            return Status::InvalidArgument(
                "legacy RaBitQ requires generic ExData layout");
        }
        info_.rabitq_config.total_bits = info_.rabitq_config.bits;
        info_.rabitq_config.ex_bits =
            info_.rabitq_config.bits > 1 ? info_.rabitq_config.bits : 0;
        info_.rabitq_config.exdata_layout = RaBitQExDataLayout::kGenericPacked;
    }
    info_.lookup_table.resize(num_clusters);
    current_cluster_index_ = 0;
    in_cluster_ = false;
    finalized_ = false;

    const uint32_t file_version = EffectiveWriterFileVersion(info_.rabitq_config);
    if (file_version >= kFileVersion && !info_.rabitq_config.uses_official_1_plus_n() &&
        info_.rabitq_config.bits > 1 &&
        !SupportsPackedStage2MagnitudeBits(info_.rabitq_config.bits)) {
        return Status::InvalidArgument(
            "Packed ExRaBitQ Stage2 magnitude supports only bits=2 or bits=4");
    }

    file_.open(path, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    if (!file_.is_open()) {
        return Status::IOError("Failed to open ClusterStore: " + path);
    }

    WriteVal(file_, kGlobalMagic);
    WriteVal(file_, file_version);
    WriteVal(file_, num_clusters);
    WriteVal(file_, dim);
    WriteVal(file_, info_.rabitq_config.bits);
    WriteVal(file_, info_.rabitq_config.block_size);
    WriteVal(file_, info_.rabitq_config.c_factor);
    if (file_version >= kFileVersionV13) {
        WriteVal(file_, info_.rabitq_config.total_bits);
        WriteVal(file_, info_.rabitq_config.ex_bits);
        WriteVal(file_, static_cast<uint8_t>(info_.rabitq_config.estimator_mode));
        if (file_version >= kFileVersionV14) {
            WriteVal(file_, static_cast<uint8_t>(info_.rabitq_config.exdata_layout));
        }
    }

    header_data_file_path_offset_ = static_cast<uint64_t>(file_.tellp());
    uint32_t zero_path_len = 0;
    WriteVal(file_, zero_path_len);
    char zero_buf[kMaxPathLen] = {0};
    WriteRaw(file_, zero_buf, kMaxPathLen);

    if (!file_.good()) {
        return Status::IOError("Failed to write global header");
    }

    lookup_table_start_ = static_cast<uint64_t>(file_.tellp());
    const uint64_t entry_sz = lookup_entry_size();
    std::vector<uint8_t> zero_entry(entry_sz, 0);
    for (uint32_t i = 0; i < num_clusters; ++i) {
        WriteRaw(file_, zero_entry.data(), entry_sz);
    }

    if (!file_.good()) {
        return Status::IOError("Failed to write lookup table placeholders");
    }

    current_offset_ = static_cast<uint64_t>(file_.tellp());
    PadTo4K(file_, current_offset_);
    return Status::OK();
}

Status ClusterStoreWriter::BeginCluster(uint32_t cluster_id,
                                        uint32_t num_records,
                                        const float* centroid,
                                        float epsilon) {
    if (!file_.is_open()) {
        return Status::InvalidArgument("ClusterStoreWriter not open");
    }
    if (in_cluster_) {
        return Status::InvalidArgument("Previous cluster not ended");
    }
    if (current_cluster_index_ >= info_.num_clusters) {
        return Status::InvalidArgument("All clusters already written");
    }

    in_cluster_ = true;
    vectors_written_ = false;
    address_written_ = false;

    auto& entry = info_.lookup_table[current_cluster_index_];
    entry.cluster_id = cluster_id;
    entry.num_records = num_records;
    entry.epsilon = epsilon;
    entry.centroid.assign(centroid, centroid + info_.dim);
    block_start_ = current_offset_;
    return Status::OK();
}

Status ClusterStoreWriter::WriteVectors(
    const std::vector<rabitq::RaBitQCode>& codes) {
    if (!file_.is_open() || !in_cluster_) {
        return Status::InvalidArgument("Not in a cluster block");
    }
    if (vectors_written_) {
        return Status::InvalidArgument("Vectors already written");
    }

    const uint32_t N = static_cast<uint32_t>(codes.size());
    const uint32_t dim = info_.dim;
    const uint32_t num_blocks = (N + 31) / 32;
    const uint32_t packed_size = FastScanPackedSize(dim);
    const uint32_t block_bytes = FastScanBlockSize(dim);

    current_num_fastscan_blocks_ = num_blocks;
    std::vector<uint8_t> packed_buf(packed_size, 0);

    for (uint32_t b = 0; b < num_blocks; ++b) {
        const uint32_t start = b * 32;
        const uint32_t count = std::min(32u, N - start);

        PackSignBitsForFastScan(&codes[start], count, dim, packed_buf.data());
        WriteRaw(file_, packed_buf.data(), packed_size);

        float norms[32] = {0};
        for (uint32_t j = 0; j < count; ++j) {
            norms[j] = codes[start + j].norm;
        }
        WriteRaw(file_, norms, 32 * sizeof(float));

        if (!file_.good()) {
            return Status::IOError("Failed to write FastScan block");
        }
        current_offset_ += block_bytes;
    }

    current_exrabitq_region_offset_ =
        static_cast<uint32_t>(current_offset_ - block_start_);

    if (info_.rabitq_config.stage2_payload_bits() > 0) {
        const uint32_t writer_version =
            EffectiveWriterFileVersion(info_.rabitq_config);
        const bool official = info_.rabitq_config.uses_official_1_plus_n();
        const uint8_t stage2_bits = info_.rabitq_config.stage2_payload_bits();
        const RaBitQExDataLayout exdata_layout =
            info_.rabitq_config.effective_exdata_layout();
        const bool official_direct =
            official && RaBitQExDataLayoutIsDirect(exdata_layout);
        if (writer_version >= kFileVersion && !official &&
            !SupportsPackedStage2MagnitudeBits(info_.rabitq_config.bits)) {
            return Status::InvalidArgument(
                "Packed ExRaBitQ Stage2 magnitude supports only bits=2 or bits=4");
        }
        const bool pack_stage2_magnitude = writer_version >= kFileVersion;
        const uint32_t sign_bytes =
            PackedSignBytes(dim);
        if (writer_version >= kFileVersionV11) {
            const uint32_t batch_size = ExRaBitQBatchSize();
            const uint32_t dim_block = ExRaBitQDimBlock();
            const uint32_t num_dim_blocks = (dim + dim_block - 1) / dim_block;
            const uint32_t num_batch_blocks = (N + batch_size - 1) / batch_size;
            const uint32_t sign_block_bytes = dim_block / 8;
            const uint32_t abs_lane_bytes = pack_stage2_magnitude
                ? simd::ExRaBitQPackedMagnitudeBytes(
                      dim_block, stage2_bits)
                : dim_block;
            if (pack_stage2_magnitude && abs_lane_bytes == 0) {
                return Status::InvalidArgument(
                    "unsupported ExRaBitQ packed Stage2 payload bit width");
            }

            if (official && UsesVariableOfficialExDataLayout(exdata_layout)) {
                if (exdata_layout == RaBitQExDataLayout::kVectorNibble4 &&
                    stage2_bits != 4) {
                    return Status::InvalidArgument(
                        "vector_nibble4 official ExData layout requires ex_bits=4");
                }
                if (exdata_layout == RaBitQExDataLayout::kVector2Bit &&
                    stage2_bits != 2) {
                    return Status::InvalidArgument(
                        "vector_2bit official ExData layout requires ex_bits=2");
                }
                if (exdata_layout == RaBitQExDataLayout::kVectorBitMajorTiles &&
                    (stage2_bits < 1 || stage2_bits > 3)) {
                    return Status::InvalidArgument(
                        "vector_bitmajor_tiles official ExData layout requires ex_bits=1,2,3");
                }
                if (exdata_layout != RaBitQExDataLayout::kVectorNibble4 &&
                    exdata_layout != RaBitQExDataLayout::kVector2Bit &&
                    exdata_layout != RaBitQExDataLayout::kVectorBitMajorTiles &&
                    !IsSmallLaneBitplanesLayout(exdata_layout) &&
                    !IsVectorBitplanesLayout(exdata_layout) && stage2_bits != 3) {
                    return Status::InvalidArgument(
                        "variable official ExData layouts currently require ex_bits=3");
                }
                std::vector<uint8_t> region;
                const uint32_t offset_count = num_batch_blocks + 1;
                const uint32_t header_bytes = 8u + offset_count * sizeof(uint32_t);
                region.resize(header_bytes, 0);
                std::memcpy(region.data(), &kVariableExRegionMagic, sizeof(uint32_t));
                std::memcpy(region.data() + 4, &num_batch_blocks, sizeof(uint32_t));

                for (uint32_t bb = 0; bb < num_batch_blocks; ++bb) {
                    const uint32_t batch_offset = static_cast<uint32_t>(region.size());
                    std::memcpy(region.data() + 8 + bb * sizeof(uint32_t),
                                &batch_offset, sizeof(uint32_t));
                    const uint32_t start = bb * batch_size;
                    const uint32_t valid_count = std::min(batch_size, N - start);
                    AppendVal<uint32_t>(region, valid_count);

                    if (IsVectorBitplanesLayout(exdata_layout)) {
                        for (uint32_t lane = 0; lane < valid_count; ++lane) {
                            const auto& code = codes[start + lane];
                            if (code.ex_code.size() != dim) {
                                return Status::InvalidArgument(
                                    "ExRaBitQ code size mismatch with dim");
                            }
                            if (!code.ex_code_sign_folded) {
                                return Status::InvalidArgument(
                                    "official ExRaBitQ vector layout requires sign-folded ExData");
                            }
                            for (uint32_t db = 0; db < num_dim_blocks; ++db) {
                                const uint32_t dim_start = db * dim_block;
                                const uint32_t copy = std::min(dim_block, dim - dim_start);
                                uint8_t abs_buf[64] = {0};
                                std::memcpy(abs_buf, code.ex_code.data() + dim_start, copy);
                                uint8_t packed_abs_buf[32] = {0};
                                if (!simd::ExRaBitQPackOfficialDirectBitplanes(
                                        abs_buf, dim_block, stage2_bits, packed_abs_buf,
                                        abs_lane_bytes)) {
                                    return Status::InvalidArgument(
                                        "ExRaBitQ code contains value outside bit-width range");
                                }
	                                AppendRaw(region, packed_abs_buf, abs_lane_bytes);
	                            }
	                        }
	                    } else if (IsVectorBitMajorTileLayout(exdata_layout)) {
	                        const uint32_t vector_bytes =
	                            simd::ExRaBitQBitMajorTileVectorBytes(dim, stage2_bits);
	                        if (vector_bytes == 0) {
	                            return Status::InvalidArgument(
	                                "unsupported vector_bitmajor_tiles Stage2 payload width");
	                        }
	                        std::vector<uint8_t> packed_abs_buf(vector_bytes);
	                        for (uint32_t lane = 0; lane < valid_count; ++lane) {
	                            const auto& code = codes[start + lane];
	                            if (code.ex_code.size() != dim) {
	                                return Status::InvalidArgument(
	                                    "ExRaBitQ code size mismatch with dim");
	                            }
	                            if (!code.ex_code_sign_folded) {
	                                return Status::InvalidArgument(
	                                    "official ExRaBitQ bit-major tile layout requires sign-folded ExData");
	                            }
	                            if (!simd::ExRaBitQPackOfficialBitMajorTiles(
	                                    code.ex_code.data(), dim, stage2_bits,
	                                    packed_abs_buf.data(), vector_bytes)) {
	                                return Status::InvalidArgument(
	                                    "ExRaBitQ code contains value outside bit-width range");
	                            }
	                            AppendRaw(region, packed_abs_buf.data(), vector_bytes);
	                        }
	                    } else if (IsSmallLaneBitplanesLayout(exdata_layout)) {
	                        const uint32_t subgroup_lanes =
	                            SmallLaneBitplanesGroupSize(exdata_layout);
	                        for (uint32_t group_start = 0; group_start < valid_count;
	                             group_start += subgroup_lanes) {
	                            const uint32_t group_lanes =
	                                std::min<uint32_t>(subgroup_lanes, valid_count - group_start);
	                            for (uint32_t db = 0; db < num_dim_blocks; ++db) {
	                                const uint32_t dim_start = db * dim_block;
	                                const uint32_t copy = std::min(dim_block, dim - dim_start);
	                                for (uint32_t local_lane = 0; local_lane < group_lanes;
	                                     ++local_lane) {
	                                    const uint32_t lane = group_start + local_lane;
	                                    const auto& code = codes[start + lane];
	                                    if (code.ex_code.size() != dim) {
	                                        return Status::InvalidArgument(
	                                            "ExRaBitQ code size mismatch with dim");
	                                    }
	                                    if (!code.ex_code_sign_folded) {
	                                        return Status::InvalidArgument(
	                                            "official ExRaBitQ small-lane layout requires sign-folded ExData");
	                                    }
	                                    uint8_t abs_buf[64] = {0};
	                                    std::memcpy(abs_buf, code.ex_code.data() + dim_start,
	                                                copy);
	                                    uint8_t packed_abs_buf[32] = {0};
	                                    if (!simd::ExRaBitQPackOfficialDirectBitplanes(
	                                            abs_buf, dim_block, stage2_bits,
	                                            packed_abs_buf, abs_lane_bytes)) {
	                                        return Status::InvalidArgument(
	                                            "ExRaBitQ code contains value outside bit-width range");
	                                    }
	                                    AppendRaw(region, packed_abs_buf, abs_lane_bytes);
	                                }
	                            }
	                        }
	                    } else if (exdata_layout == RaBitQExDataLayout::kVector2Bit) {
	                        for (uint32_t lane = 0; lane < valid_count; ++lane) {
	                            const auto& code = codes[start + lane];
                            if (code.ex_code.size() != dim) {
                                return Status::InvalidArgument(
                                    "ExRaBitQ code size mismatch with dim");
                            }
                            if (!code.ex_code_sign_folded) {
                                return Status::InvalidArgument(
                                    "official ExRaBitQ vector 2-bit layout requires sign-folded ExData");
                            }
                            for (uint32_t db = 0; db < num_dim_blocks; ++db) {
                                const uint32_t dim_start = db * dim_block;
                                const uint32_t copy = std::min(dim_block, dim - dim_start);
                                uint8_t abs_buf[64] = {0};
                                std::memcpy(abs_buf, code.ex_code.data() + dim_start, copy);
                                uint8_t packed_abs_buf[32] = {0};
                                if (!simd::ExRaBitQPackOfficial2Bit(
                                        abs_buf, dim_block, packed_abs_buf, abs_lane_bytes)) {
                                    return Status::InvalidArgument(
                                        "ExRaBitQ code contains value outside 2-bit range");
                                }
                                AppendRaw(region, packed_abs_buf, abs_lane_bytes);
                            }
                        }
                    } else if (exdata_layout == RaBitQExDataLayout::kVectorNibble4) {
                        for (uint32_t lane = 0; lane < valid_count; ++lane) {
                            const auto& code = codes[start + lane];
                            if (code.ex_code.size() != dim) {
                                return Status::InvalidArgument(
                                    "ExRaBitQ code size mismatch with dim");
                            }
                            if (!code.ex_code_sign_folded) {
                                return Status::InvalidArgument(
                                    "official ExRaBitQ vector nibble layout requires sign-folded ExData");
                            }
                            for (uint32_t db = 0; db < num_dim_blocks; ++db) {
                                const uint32_t dim_start = db * dim_block;
                                const uint32_t copy = std::min(dim_block, dim - dim_start);
                                uint8_t abs_buf[64] = {0};
                                std::memcpy(abs_buf, code.ex_code.data() + dim_start, copy);
                                uint8_t packed_abs_buf[32] = {0};
                                if (!simd::ExRaBitQPackOfficialNibble4(
                                        abs_buf, dim_block, packed_abs_buf, abs_lane_bytes)) {
                                    return Status::InvalidArgument(
                                        "ExRaBitQ code contains value outside 4-bit range");
                                }
                                AppendRaw(region, packed_abs_buf, abs_lane_bytes);
                            }
                        }
                    } else if (exdata_layout == RaBitQExDataLayout::kSplit3TrimmedBitplanes) {
                        for (uint32_t db = 0; db < num_dim_blocks; ++db) {
                            const uint32_t dim_start = db * dim_block;
                            for (uint32_t lane = 0; lane < valid_count; ++lane) {
                                uint8_t abs_buf[64] = {0};
                                const auto& code = codes[start + lane];
                                if (code.ex_code.size() != dim) {
                                    return Status::InvalidArgument(
                                        "ExRaBitQ code size mismatch with dim");
                                }
                                if (!code.ex_code_sign_folded) {
                                    return Status::InvalidArgument(
                                        "official ExRaBitQ v15 requires sign-folded ExData");
                                }
                                const uint32_t copy = std::min(dim_block, dim - dim_start);
                                std::memcpy(abs_buf, code.ex_code.data() + dim_start, copy);
                                uint8_t packed_abs_buf[32] = {0};
                                if (!simd::ExRaBitQPackOfficialDirectBitplanes(
                                        abs_buf, dim_block, stage2_bits, packed_abs_buf,
                                        abs_lane_bytes)) {
                                    return Status::InvalidArgument(
                                        "ExRaBitQ code contains value outside bit-width range");
                                }
                                AppendRaw(region, packed_abs_buf, abs_lane_bytes);
                            }
                        }
                    } else if (exdata_layout == RaBitQExDataLayout::kSplit3ZeroPlaneElide) {
                        for (uint32_t db = 0; db < num_dim_blocks; ++db) {
                            const uint32_t dim_start = db * dim_block;
                            uint64_t lane_words[8][3] = {};
                            for (uint32_t lane = 0; lane < valid_count; ++lane) {
                                uint8_t abs_buf[64] = {0};
                                const auto& code = codes[start + lane];
                                if (code.ex_code.size() != dim) {
                                    return Status::InvalidArgument(
                                        "ExRaBitQ code size mismatch with dim");
                                }
                                if (!code.ex_code_sign_folded) {
                                    return Status::InvalidArgument(
                                        "official ExRaBitQ v15 requires sign-folded ExData");
                                }
                                const uint32_t copy = std::min(dim_block, dim - dim_start);
                                std::memcpy(abs_buf, code.ex_code.data() + dim_start, copy);
                                uint8_t packed_abs_buf[32] = {0};
                                if (!simd::ExRaBitQPackOfficialDirectBitplanes(
                                        abs_buf, dim_block, stage2_bits, packed_abs_buf,
                                        abs_lane_bytes)) {
                                    return Status::InvalidArgument(
                                        "ExRaBitQ code contains value outside bit-width range");
                                }
                                for (uint32_t plane = 0; plane < stage2_bits; ++plane) {
                                    std::memcpy(&lane_words[lane][plane],
                                                packed_abs_buf + plane * sizeof(uint64_t),
                                                sizeof(uint64_t));
                                }
                            }
                            for (uint32_t plane = 0; plane < stage2_bits; ++plane) {
                                uint8_t nonzero_mask = 0;
                                for (uint32_t lane = 0; lane < valid_count; ++lane) {
                                    if (lane_words[lane][plane] != 0) {
                                        nonzero_mask |= static_cast<uint8_t>(1u << lane);
                                    }
                                }
                                AppendVal<uint8_t>(region, nonzero_mask);
                                for (uint32_t lane = 0; lane < valid_count; ++lane) {
                                    if ((nonzero_mask & (1u << lane)) != 0) {
                                        AppendVal<uint64_t>(region, lane_words[lane][plane]);
                                    }
                                }
                            }
                        }
                    }

                    for (uint32_t lane = 0; lane < valid_count; ++lane) {
                        AppendVal<float>(region, codes[start + lane].ex_factor_add);
                    }
                    for (uint32_t lane = 0; lane < valid_count; ++lane) {
                        AppendVal<float>(region, codes[start + lane].ex_factor_rescale);
                    }
                }
                const uint32_t end_offset = static_cast<uint32_t>(region.size());
                std::memcpy(region.data() + 8 + num_batch_blocks * sizeof(uint32_t),
                            &end_offset, sizeof(uint32_t));
                WriteRaw(file_, region.data(), region.size());
                if (!file_.good()) {
                    return Status::IOError("Failed to write variable official ExRaBitQ region");
                }
                current_offset_ += region.size();
                vectors_written_ = true;
                return Status::OK();
            }

            for (uint32_t bb = 0; bb < num_batch_blocks; ++bb) {
                const uint32_t start = bb * batch_size;
                const uint32_t valid_count = std::min(batch_size, N - start);
                WriteVal(file_, valid_count);

                for (uint32_t db = 0; db < num_dim_blocks; ++db) {
                    const uint32_t dim_start = db * dim_block;
                    for (uint32_t lane = 0; lane < batch_size; ++lane) {
                        uint8_t abs_buf[64] = {0};
                        if (lane < valid_count) {
                            const auto& code = codes[start + lane];
                            if (code.ex_code.size() != dim) {
                                return Status::InvalidArgument(
                                    "ExRaBitQ code size mismatch with dim");
                            }
                            if (official && !code.ex_code_sign_folded) {
                                return Status::InvalidArgument(
                                    "official ExRaBitQ v13 requires sign-folded ExData");
                            }
                            if (!official &&
                                code.ex_sign_packed.size() != sign_bytes) {
                                return Status::InvalidArgument(
                                    "ExRaBitQ code/sign size mismatch with dim");
                            }
                            const uint32_t copy = std::min(dim_block, dim - dim_start);
                            std::memcpy(abs_buf, code.ex_code.data() + dim_start, copy);
                        }
                        if (pack_stage2_magnitude) {
                            uint8_t packed_abs_buf[32] = {0};
                            bool packed_ok = false;
                            if (official_direct) {
                                packed_ok =
                                    exdata_layout == RaBitQExDataLayout::kSplit3TwoPlusOne
                                        ? simd::ExRaBitQPackOfficialDirect3(
                                              abs_buf, dim_block, exdata_layout,
                                              packed_abs_buf, abs_lane_bytes)
                                        : simd::ExRaBitQPackOfficialDirectBitplanes(
                                              abs_buf, dim_block, stage2_bits,
                                              packed_abs_buf, abs_lane_bytes);
                            } else {
                                packed_ok = simd::ExRaBitQPackMagnitudes(
                                    abs_buf, dim_block, stage2_bits,
                                    packed_abs_buf, abs_lane_bytes);
                            }
                            if (!packed_ok) {
                                return Status::InvalidArgument(
                                    "ExRaBitQ code contains value outside bit-width range");
                            }
                            WriteRaw(file_, packed_abs_buf, abs_lane_bytes);
                        } else {
                            WriteRaw(file_, abs_buf, dim_block);
                        }
                    }
                }
                if (!official) {
                    for (uint32_t db = 0; db < num_dim_blocks; ++db) {
                        const uint32_t dim_start = db * dim_block;
                        const uint32_t sign_offset = dim_start / 8;
                        const uint32_t sign_copy =
                            sign_offset < sign_bytes
                                ? std::min(sign_block_bytes, sign_bytes - sign_offset)
                                : 0;
                        for (uint32_t lane = 0; lane < batch_size; ++lane) {
                            uint8_t sign_buf[8] = {0};
                            if (lane < valid_count && sign_copy > 0) {
                                const auto& code = codes[start + lane];
                                const uint8_t* src = code.ex_sign_packed.data() + sign_offset;
                                std::memcpy(sign_buf, src, sign_copy);
                            }
                            WriteRaw(file_, sign_buf, sign_block_bytes);
                        }
                    }
                }
                if (official) {
                    for (uint32_t lane = 0; lane < batch_size; ++lane) {
                        const float factor_add =
                            (lane < valid_count) ? codes[start + lane].ex_factor_add : 0.0f;
                        WriteVal(file_, factor_add);
                    }
                    for (uint32_t lane = 0; lane < batch_size; ++lane) {
                        const float factor_rescale =
                            (lane < valid_count)
                                ? codes[start + lane].ex_factor_rescale
                                : 0.0f;
                        WriteVal(file_, factor_rescale);
                    }
                } else {
                    for (uint32_t lane = 0; lane < batch_size; ++lane) {
                        const float xipnorm =
                            (lane < valid_count) ? codes[start + lane].xipnorm : 0.0f;
                        WriteVal(file_, xipnorm);
                    }
                }
                if (!file_.good()) {
                    return Status::IOError("Failed to write ExRaBitQ compact block");
                }
                const uint32_t block_bytes =
                    sizeof(uint32_t) +
                    num_dim_blocks * batch_size * abs_lane_bytes +
                    (official ? 0 : num_dim_blocks * batch_size * sign_block_bytes) +
                    (official ? batch_size * 2u : batch_size) * sizeof(float);
                current_offset_ += block_bytes;
            }
        } else {
            for (const auto& code : codes) {
                if (code.ex_code.size() != dim ||
                    code.ex_sign_packed.size() != sign_bytes) {
                    return Status::InvalidArgument(
                        "ExRaBitQ code/sign size mismatch with dim");
                }
                WriteRaw(file_, code.ex_code.data(), dim);
                WriteRaw(file_, code.ex_sign_packed.data(), sign_bytes);
                WriteVal(file_, code.xipnorm);
                if (!file_.good()) {
                    return Status::IOError("Failed to write ExRaBitQ entry");
                }
                current_offset_ += dim + sign_bytes + sizeof(float);
            }
        }
    }

    vectors_written_ = true;
    return Status::OK();
}

Status ClusterStoreWriter::WriteAddressBlocks(
    const EncodedAddressColumn& column) {
    if (!file_.is_open() || !in_cluster_) {
        return Status::InvalidArgument("Not in a cluster block");
    }
    if (!vectors_written_) {
        return Status::InvalidArgument("Must write vectors before address blocks");
    }
    if (address_written_) {
        return Status::InvalidArgument("Address blocks already written");
    }

    const auto& entry = info_.lookup_table[current_cluster_index_];
    if (column.total_records != entry.num_records) {
        return Status::InvalidArgument("Address column record count does not match cluster");
    }
    if (column.format == AddressFormat::V1Packed) {
        if (column.blocks.size() != column.layout.num_address_blocks) {
            return Status::InvalidArgument("Address column block count does not match layout");
        }
        if (entry.num_records > 0 && column.layout.num_address_blocks == 0) {
            return Status::InvalidArgument("Non-empty cluster requires address blocks");
        }
    } else {
        if (column.raw_entries.size() != entry.num_records) {
            return Status::InvalidArgument("Raw address table entry count mismatch");
        }
    }

    current_address_column_ = column;
    if (current_address_column_.format == AddressFormat::V1Packed) {
        std::vector<AddressEntry> decoded;
        VDB_RETURN_IF_ERROR(AddressColumn::Decode(current_address_column_, decoded));
        current_address_column_ = AddressColumn::EncodeRawTableV2(
            decoded, current_address_column_.layout.page_size);
    }

    const size_t bytes =
        current_address_column_.raw_entries.size() * sizeof(RawAddressEntryV2);
    if (bytes > 0) {
        file_.write(reinterpret_cast<const char*>(current_address_column_.raw_entries.data()),
                    static_cast<std::streamsize>(bytes));
        if (!file_.good()) {
            return Status::IOError("Failed to write raw address table");
        }
        current_offset_ += bytes;
    }

    address_written_ = true;
    return Status::OK();
}

Status ClusterStoreWriter::EndCluster() {
    if (!file_.is_open() || !in_cluster_) {
        return Status::InvalidArgument("Not in a cluster block");
    }
    if (!address_written_) {
        return Status::InvalidArgument("Must write address blocks before EndCluster");
    }

    const uint64_t trailer_start = current_offset_;
    const uint32_t address_entry_size = sizeof(RawAddressEntryV2);
    const uint32_t num_entries =
        static_cast<uint32_t>(current_address_column_.raw_entries.size());
    const uint32_t address_payload_bytes = num_entries * address_entry_size;
    const uint32_t address_payload_offset = static_cast<uint32_t>(
        trailer_start - block_start_ - address_payload_bytes);

    WriteVal(file_, kAddressFormatV2);
    WriteVal(file_, current_address_column_.layout.page_size);
    WriteVal(file_, address_entry_size);
    WriteVal(file_, num_entries);
    WriteVal(file_, address_payload_offset);
    WriteVal(file_, address_payload_bytes);

    const uint64_t after_blocks_pos = static_cast<uint64_t>(file_.tellp());
    const uint32_t mini_trailer_size =
        static_cast<uint32_t>((after_blocks_pos + 8) - trailer_start);
    WriteVal(file_, mini_trailer_size);
    WriteVal(file_, kBlockMagic);

    if (!file_.good()) {
        return Status::IOError("Failed to write block mini-trailer");
    }

    current_offset_ = static_cast<uint64_t>(file_.tellp());
    auto& entry = info_.lookup_table[current_cluster_index_];
    entry.block_offset = block_start_;
    entry.block_size = current_offset_ - block_start_;
    entry.num_fastscan_blocks = current_num_fastscan_blocks_;
    entry.exrabitq_region_offset = current_exrabitq_region_offset_;

    const uint64_t entry_offset =
        lookup_table_start_ +
        static_cast<uint64_t>(current_cluster_index_) * lookup_entry_size();
    file_.seekp(static_cast<std::streamoff>(entry_offset));

    WriteVal(file_, entry.cluster_id);
    WriteVal(file_, entry.num_records);
    WriteVal(file_, entry.epsilon);
    file_.write(reinterpret_cast<const char*>(entry.centroid.data()),
                static_cast<std::streamsize>(info_.dim * sizeof(float)));
    WriteVal(file_, entry.block_offset);
    WriteVal(file_, entry.block_size);
    WriteVal(file_, entry.num_fastscan_blocks);
    WriteVal(file_, entry.exrabitq_region_offset);

    if (!file_.good()) {
        return Status::IOError("Failed to patch lookup table entry");
    }

    file_.seekp(static_cast<std::streamoff>(current_offset_));
    PadTo4K(file_, current_offset_);

    current_cluster_index_++;
    in_cluster_ = false;
    vectors_written_ = false;
    address_written_ = false;
    current_address_column_ = EncodedAddressColumn{};
    current_num_fastscan_blocks_ = 0;
    current_exrabitq_region_offset_ = 0;
    return Status::OK();
}

Status ClusterStoreWriter::Finalize(const std::string& data_file_path) {
    if (!file_.is_open()) {
        return Status::InvalidArgument("ClusterStoreWriter not open");
    }
    if (finalized_) {
        return Status::InvalidArgument("Already finalized");
    }
    if (in_cluster_) {
        return Status::InvalidArgument("Cluster not ended");
    }

    info_.data_file_path = data_file_path;
    file_.seekp(static_cast<std::streamoff>(header_data_file_path_offset_));
    const uint32_t path_len = static_cast<uint32_t>(data_file_path.size());
    if (path_len > kMaxPathLen) {
        return Status::InvalidArgument("data_file_path exceeds max length");
    }
    WriteVal(file_, path_len);
    char path_buf[kMaxPathLen] = {0};
    std::memcpy(path_buf, data_file_path.data(), path_len);
    WriteRaw(file_, path_buf, kMaxPathLen);

    if (!file_.good()) {
        return Status::IOError("Failed to patch data_file_path");
    }

    file_.flush();
    file_.close();
    finalized_ = true;
    return Status::OK();
}

ClusterStoreReader::ClusterStoreReader() = default;

ClusterStoreReader::~ClusterStoreReader() {
    Close();
}

Status ClusterStoreReader::AllocateResidentMmapFileBuffer(uint64_t payload_size) {
    ReleaseResidentMmapFileBuffer();
    if (payload_size == 0) {
        resident_mmap_data_bytes_ = 0;
        return Status::OK();
    }
    static constexpr uint64_t kHugePageBytes = 2ull << 20;
    const uint64_t mapping_bytes = payload_size + kHugePageBytes - 1u;
    void* base = ::mmap(nullptr, static_cast<size_t>(mapping_bytes),
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        return Status::IOError("Failed to mmap resident .clu buffer");
    }
    const uintptr_t base_addr = reinterpret_cast<uintptr_t>(base);
    const uintptr_t aligned_addr = AlignUp(base_addr, kHugePageBytes);
    resident_mmap_base_ = static_cast<uint8_t*>(base);
    resident_mmap_mapping_bytes_ = mapping_bytes;
    resident_mmap_data_ = reinterpret_cast<uint8_t*>(aligned_addr);
    resident_mmap_data_bytes_ = payload_size;
    return Status::OK();
}

Status ClusterStoreReader::AllocateResidentCodeSlabBuffer(uint64_t payload_size) {
    ReleaseResidentCodeSlabBuffer();
    if (payload_size == 0) {
        resident_code_slab_data_bytes_ = 0;
        return Status::OK();
    }
    static constexpr uint64_t kHugePageBytes = 2ull << 20;
    const uint64_t mapping_bytes = payload_size + kHugePageBytes - 1u;
    void* base = ::mmap(nullptr, static_cast<size_t>(mapping_bytes),
                        PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (base == MAP_FAILED) {
        return Status::IOError("Failed to mmap resident code slab buffer");
    }
    const uintptr_t base_addr = reinterpret_cast<uintptr_t>(base);
    const uintptr_t aligned_addr = AlignUp(base_addr, kHugePageBytes);
    resident_code_slab_base_ = static_cast<uint8_t*>(base);
    resident_code_slab_mapping_bytes_ = mapping_bytes;
    resident_code_slab_data_ = reinterpret_cast<uint8_t*>(aligned_addr);
    resident_code_slab_data_bytes_ = payload_size;
    return Status::OK();
}

void ClusterStoreReader::ReleaseResidentMmapFileBuffer() {
    if (resident_mmap_base_ != nullptr) {
        (void)::munmap(resident_mmap_base_,
                       static_cast<size_t>(resident_mmap_mapping_bytes_));
    }
    resident_mmap_base_ = nullptr;
    resident_mmap_data_ = nullptr;
    resident_mmap_mapping_bytes_ = 0;
    resident_mmap_data_bytes_ = 0;
}

void ClusterStoreReader::ReleaseResidentCodeSlabBuffer() {
    if (resident_code_slab_base_ != nullptr) {
        (void)::munmap(resident_code_slab_base_,
                       static_cast<size_t>(resident_code_slab_mapping_bytes_));
    }
    resident_code_slab_base_ = nullptr;
    resident_code_slab_data_ = nullptr;
    resident_code_slab_mapping_bytes_ = 0;
    resident_code_slab_data_bytes_ = 0;
}

uint8_t* ClusterStoreReader::ResidentFileBufferData() {
    if (resident_mmap_data_ != nullptr) {
        return resident_mmap_data_;
    }
    return resident_file_buffer_.empty() ? nullptr : resident_file_buffer_.data();
}

const uint8_t* ClusterStoreReader::ResidentFileBufferData() const {
    if (resident_mmap_data_ != nullptr) {
        return resident_mmap_data_;
    }
    return resident_file_buffer_.empty() ? nullptr : resident_file_buffer_.data();
}

uint64_t ClusterStoreReader::ResidentFileBufferSize() const {
    if (resident_mmap_data_ != nullptr) {
        return resident_mmap_data_bytes_;
    }
    return static_cast<uint64_t>(resident_file_buffer_.size());
}

uint8_t* ClusterStoreReader::ResidentCodeSlabData() {
    return resident_code_slab_data_;
}

const uint8_t* ClusterStoreReader::ResidentCodeSlabData() const {
    return resident_code_slab_data_;
}

ClusterStoreReader::ClusterStoreReader(ClusterStoreReader&& other) noexcept
    : fd_(other.fd_),
      info_(std::move(other.info_)),
      cluster_index_(std::move(other.cluster_index_)),
      loaded_clusters_(std::move(other.loaded_clusters_)),
      resident_file_buffer_(std::move(other.resident_file_buffer_)),
      resident_mmap_base_(other.resident_mmap_base_),
      resident_mmap_data_(other.resident_mmap_data_),
      resident_mmap_mapping_bytes_(other.resident_mmap_mapping_bytes_),
      resident_mmap_data_bytes_(other.resident_mmap_data_bytes_),
      resident_code_slab_base_(other.resident_code_slab_base_),
      resident_code_slab_data_(other.resident_code_slab_data_),
      resident_code_slab_mapping_bytes_(other.resident_code_slab_mapping_bytes_),
      resident_code_slab_data_bytes_(other.resident_code_slab_data_bytes_),
      resident_clusters_(std::move(other.resident_clusters_)),
      resident_parsed_clusters_(std::move(other.resident_parsed_clusters_)),
      resident_preload_ready_(other.resident_preload_ready_),
      resident_preload_bytes_(other.resident_preload_bytes_),
      resident_cluster_mem_bytes_(other.resident_cluster_mem_bytes_),
      resident_file_size_bytes_(other.resident_file_size_bytes_),
      resident_file_buffer_bytes_(other.resident_file_buffer_bytes_),
      resident_code_storage_bytes_(other.resident_code_storage_bytes_),
      resident_decoded_address_bytes_(other.resident_decoded_address_bytes_),
      resident_raw_address_bytes_(other.resident_raw_address_bytes_),
      resident_parsed_address_duplicate_bytes_(
          other.resident_parsed_address_duplicate_bytes_),
      resident_preload_batch_size_(other.resident_preload_batch_size_),
      resident_preload_mode_(std::move(other.resident_preload_mode_)),
      resident_preload_time_ms_(other.resident_preload_time_ms_),
      resident_parallel_view_bytes_(other.resident_parallel_view_bytes_),
      resident_parallel_view_build_ms_(other.resident_parallel_view_build_ms_),
      resident_stage1_envelope_bytes_(other.resident_stage1_envelope_bytes_) {
    other.fd_ = -1;
    other.resident_mmap_base_ = nullptr;
    other.resident_mmap_data_ = nullptr;
    other.resident_mmap_mapping_bytes_ = 0;
    other.resident_mmap_data_bytes_ = 0;
    other.resident_code_slab_base_ = nullptr;
    other.resident_code_slab_data_ = nullptr;
    other.resident_code_slab_mapping_bytes_ = 0;
    other.resident_code_slab_data_bytes_ = 0;
    other.resident_preload_ready_ = false;
    other.resident_preload_bytes_ = 0;
    other.resident_cluster_mem_bytes_ = 0;
    other.resident_file_size_bytes_ = 0;
    other.resident_file_buffer_bytes_ = 0;
    other.resident_code_storage_bytes_ = 0;
    other.resident_decoded_address_bytes_ = 0;
    other.resident_raw_address_bytes_ = 0;
    other.resident_parsed_address_duplicate_bytes_ = 0;
    other.resident_preload_batch_size_ = 0;
    other.resident_preload_mode_.clear();
    other.resident_preload_time_ms_ = 0.0;
    other.resident_parallel_view_bytes_ = 0;
    other.resident_parallel_view_build_ms_ = 0.0;
    other.resident_stage1_envelope_bytes_ = 0;
}

ClusterStoreReader& ClusterStoreReader::operator=(
    ClusterStoreReader&& other) noexcept {
    if (this != &other) {
        Close();
        fd_ = other.fd_;
        info_ = std::move(other.info_);
        cluster_index_ = std::move(other.cluster_index_);
        loaded_clusters_ = std::move(other.loaded_clusters_);
        resident_file_buffer_ = std::move(other.resident_file_buffer_);
        resident_mmap_base_ = other.resident_mmap_base_;
        resident_mmap_data_ = other.resident_mmap_data_;
        resident_mmap_mapping_bytes_ = other.resident_mmap_mapping_bytes_;
        resident_mmap_data_bytes_ = other.resident_mmap_data_bytes_;
        resident_code_slab_base_ = other.resident_code_slab_base_;
        resident_code_slab_data_ = other.resident_code_slab_data_;
        resident_code_slab_mapping_bytes_ = other.resident_code_slab_mapping_bytes_;
        resident_code_slab_data_bytes_ = other.resident_code_slab_data_bytes_;
        resident_clusters_ = std::move(other.resident_clusters_);
        resident_parsed_clusters_ = std::move(other.resident_parsed_clusters_);
        resident_preload_ready_ = other.resident_preload_ready_;
        resident_preload_bytes_ = other.resident_preload_bytes_;
        resident_cluster_mem_bytes_ = other.resident_cluster_mem_bytes_;
        resident_file_size_bytes_ = other.resident_file_size_bytes_;
        resident_file_buffer_bytes_ = other.resident_file_buffer_bytes_;
        resident_code_storage_bytes_ = other.resident_code_storage_bytes_;
        resident_decoded_address_bytes_ = other.resident_decoded_address_bytes_;
        resident_raw_address_bytes_ = other.resident_raw_address_bytes_;
        resident_parsed_address_duplicate_bytes_ =
            other.resident_parsed_address_duplicate_bytes_;
        resident_preload_batch_size_ = other.resident_preload_batch_size_;
        resident_preload_mode_ = std::move(other.resident_preload_mode_);
        resident_preload_time_ms_ = other.resident_preload_time_ms_;
        resident_parallel_view_bytes_ = other.resident_parallel_view_bytes_;
        resident_parallel_view_build_ms_ = other.resident_parallel_view_build_ms_;
        resident_stage1_envelope_bytes_ = other.resident_stage1_envelope_bytes_;
        other.fd_ = -1;
        other.resident_mmap_base_ = nullptr;
        other.resident_mmap_data_ = nullptr;
        other.resident_mmap_mapping_bytes_ = 0;
        other.resident_mmap_data_bytes_ = 0;
        other.resident_code_slab_base_ = nullptr;
        other.resident_code_slab_data_ = nullptr;
        other.resident_code_slab_mapping_bytes_ = 0;
        other.resident_code_slab_data_bytes_ = 0;
        other.resident_preload_ready_ = false;
        other.resident_preload_bytes_ = 0;
        other.resident_cluster_mem_bytes_ = 0;
        other.resident_file_size_bytes_ = 0;
        other.resident_file_buffer_bytes_ = 0;
        other.resident_code_storage_bytes_ = 0;
        other.resident_decoded_address_bytes_ = 0;
        other.resident_raw_address_bytes_ = 0;
        other.resident_parsed_address_duplicate_bytes_ = 0;
        other.resident_preload_batch_size_ = 0;
        other.resident_preload_mode_.clear();
        other.resident_preload_time_ms_ = 0.0;
        other.resident_parallel_view_bytes_ = 0;
        other.resident_parallel_view_build_ms_ = 0.0;
        other.resident_stage1_envelope_bytes_ = 0;
    }
    return *this;
}

void ClusterStoreReader::Close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    loaded_clusters_.clear();
    resident_clusters_.clear();
    resident_parsed_clusters_.clear();
    resident_file_buffer_.clear();
    ReleaseResidentMmapFileBuffer();
    ReleaseResidentCodeSlabBuffer();
    resident_preload_ready_ = false;
    resident_preload_bytes_ = 0;
    resident_cluster_mem_bytes_ = 0;
    resident_file_size_bytes_ = 0;
    resident_file_buffer_bytes_ = 0;
    resident_code_storage_bytes_ = 0;
    resident_decoded_address_bytes_ = 0;
    resident_raw_address_bytes_ = 0;
    resident_parsed_address_duplicate_bytes_ = 0;
    resident_preload_batch_size_ = 0;
    resident_preload_mode_.clear();
    resident_preload_time_ms_ = 0.0;
    resident_parallel_view_bytes_ = 0;
    resident_parallel_view_build_ms_ = 0.0;
    resident_stage1_envelope_bytes_ = 0;
    cluster_index_.clear();
    file_version_ = 0;
}

Status ClusterStoreReader::Open(const std::string& path, bool use_direct_io) {
    if (fd_ >= 0) {
        return Status::InvalidArgument("ClusterStoreReader already open");
    }

    int flags = O_RDONLY;
    if (use_direct_io) flags |= O_DIRECT;
    fd_ = ::open(path.c_str(), flags);
    if (fd_ < 0) {
        return Status::IOError("Failed to open ClusterStore: " + path);
    }

    {
        uint8_t* hdr_buf = static_cast<uint8_t*>(
            std::aligned_alloc(kAlignSize, kAlignSize));
        if (!hdr_buf) {
            Close();
            return Status::IOError("aligned_alloc failed for header");
        }
        const ssize_t n = ::pread(fd_, hdr_buf, kAlignSize, 0);
        if (n < 285) {
            std::free(hdr_buf);
            Close();
            return Status::IOError("Failed to read header");
        }

        const uint8_t* p = hdr_buf;
        uint32_t magic = 0;
        uint32_t version = 0;
        std::memcpy(&magic, p, 4);
        p += 4;
        if (magic != kGlobalMagic) {
            std::free(hdr_buf);
            Close();
            return Status::Corruption("Invalid ClusterStore magic");
        }
        std::memcpy(&version, p, 4);
        p += 4;
        if (version != kFileVersionV15 &&
            version != kFileVersionV14 &&
            version != kFileVersionV13 &&
            version != kFileVersion && version != kFileVersionV11 &&
            version != kFileVersionV10 && version != kFileVersionV9 &&
            version != kFileVersionV8 &&
            version != kFileVersionV7) {
            std::free(hdr_buf);
            Close();
            return Status::NotSupported(
                "Unsupported ClusterStore version: " + std::to_string(version));
        }
        file_version_ = version;

        std::memcpy(&info_.num_clusters, p, 4);
        p += 4;
        std::memcpy(&info_.dim, p, 4);
        p += 4;
        std::memcpy(&info_.rabitq_config.bits, p, 1);
        p += 1;
        std::memcpy(&info_.rabitq_config.block_size, p, 4);
        p += 4;
        std::memcpy(&info_.rabitq_config.c_factor, p, 4);
        p += 4;
        info_.rabitq_config.storage_version = static_cast<uint8_t>(version);
        if (version >= kFileVersionV13) {
            std::memcpy(&info_.rabitq_config.total_bits, p, 1);
            p += 1;
            std::memcpy(&info_.rabitq_config.ex_bits, p, 1);
            p += 1;
            uint8_t estimator_mode = 0;
            std::memcpy(&estimator_mode, p, 1);
            p += 1;
            info_.rabitq_config.estimator_mode =
                RaBitQEstimatorModeFromByte(estimator_mode);
            info_.rabitq_config.exdata_layout = RaBitQExDataLayout::kGenericPacked;
            if (version >= kFileVersionV14) {
                uint8_t exdata_layout = 0;
                std::memcpy(&exdata_layout, p, 1);
                p += 1;
                if (!RaBitQExDataLayoutByteValid(exdata_layout)) {
                    std::free(hdr_buf);
                    Close();
                    return Status::Corruption("unknown RaBitQ ExData layout byte");
                }
                info_.rabitq_config.exdata_layout =
                    RaBitQExDataLayoutFromByte(exdata_layout);
            }
            if (!info_.rabitq_config.uses_official_1_plus_n()) {
                std::free(hdr_buf);
                Close();
                return Status::Corruption("v13 ClusterStore requires official_1_plus_n mode");
            }
            if (!info_.rabitq_config.official_bits_valid()) {
                std::free(hdr_buf);
                Close();
                return Status::Corruption(
                    "official RaBitQ metadata violates total_bits == ex_bits + 1");
            }
            if (!SupportsOfficialExDataBits(info_.rabitq_config.ex_bits)) {
                std::free(hdr_buf);
                Close();
                return Status::NotSupported(
                    "official RaBitQ ExData supports only ex_bits=0,1,2,3,4");
            }
            if (!info_.rabitq_config.exdata_layout_valid()) {
                std::free(hdr_buf);
                Close();
                return Status::Corruption(
                    "official RaBitQ ExData layout is incompatible with ex_bits");
            }
        } else {
            info_.rabitq_config.estimator_mode =
                RaBitQEstimatorMode::kLegacySignedMagnitude;
            info_.rabitq_config.total_bits = info_.rabitq_config.bits;
            info_.rabitq_config.ex_bits =
                info_.rabitq_config.bits > 1 ? info_.rabitq_config.bits : 0;
            info_.rabitq_config.exdata_layout = RaBitQExDataLayout::kGenericPacked;
        }
        if (version >= kFileVersion && version < kFileVersionV13 &&
            info_.rabitq_config.bits > 1 &&
            !SupportsPackedStage2MagnitudeBits(info_.rabitq_config.bits)) {
            std::free(hdr_buf);
            Close();
            return Status::NotSupported(
                "Packed ExRaBitQ Stage2 magnitude supports only bits=2 or bits=4");
        }

        uint32_t path_len = 0;
        std::memcpy(&path_len, p, 4);
        p += 4;
        if (path_len > kMaxPathLen) {
            std::free(hdr_buf);
            Close();
            return Status::Corruption("data_file_path length exceeds max");
        }
        info_.data_file_path.assign(reinterpret_cast<const char*>(p), path_len);
        p += kMaxPathLen;

        off_t pos = static_cast<off_t>(p - hdr_buf);
        std::free(hdr_buf);

        info_.lookup_table.resize(info_.num_clusters);
        cluster_index_.clear();

        const uint64_t entry_sz =
            4 + 4 + 4 + static_cast<uint64_t>(info_.dim) * 4 + 8 + 8 + 4 + 4;
        const uint64_t total_lookup_size =
            static_cast<uint64_t>(info_.num_clusters) * entry_sz;
        const uint64_t aligned_lookup_size = RoundUp4K(total_lookup_size);
        uint8_t* lookup_raw = static_cast<uint8_t*>(
            std::aligned_alloc(kAlignSize, aligned_lookup_size));
        if (!lookup_raw) {
            Close();
            return Status::IOError("aligned_alloc failed for lookup");
        }

        const off_t aligned_pos = pos & ~(static_cast<off_t>(kAlignSize) - 1);
        const off_t pos_delta = pos - aligned_pos;
        const uint64_t aligned_read =
            RoundUp4K(total_lookup_size + static_cast<uint64_t>(pos_delta));
        uint8_t* aligned_read_buf = static_cast<uint8_t*>(
            std::aligned_alloc(kAlignSize, aligned_read));
        if (!aligned_read_buf) {
            std::free(lookup_raw);
            Close();
            return Status::IOError("aligned_alloc failed");
        }
        const ssize_t nr = ::pread(fd_, aligned_read_buf, aligned_read, aligned_pos);
        if (nr < static_cast<ssize_t>(total_lookup_size + pos_delta)) {
            std::free(aligned_read_buf);
            std::free(lookup_raw);
            Close();
            return Status::IOError("Failed to bulk read lookup table");
        }
        std::memcpy(lookup_raw, aligned_read_buf + pos_delta, total_lookup_size);
        std::free(aligned_read_buf);

        const uint8_t* ptr = lookup_raw;
        for (uint32_t i = 0; i < info_.num_clusters; ++i) {
            auto& entry = info_.lookup_table[i];
            std::memcpy(&entry.cluster_id, ptr, 4);
            ptr += 4;
            std::memcpy(&entry.num_records, ptr, 4);
            ptr += 4;
            std::memcpy(&entry.epsilon, ptr, 4);
            ptr += 4;
            entry.centroid.resize(info_.dim);
            std::memcpy(entry.centroid.data(), ptr, info_.dim * sizeof(float));
            ptr += info_.dim * sizeof(float);
            std::memcpy(&entry.block_offset, ptr, 8);
            ptr += 8;
            std::memcpy(&entry.block_size, ptr, 8);
            ptr += 8;
            std::memcpy(&entry.num_fastscan_blocks, ptr, 4);
            ptr += 4;
            std::memcpy(&entry.exrabitq_region_offset, ptr, 4);
            ptr += 4;
            cluster_index_[entry.cluster_id] = i;
        }
        std::free(lookup_raw);
        pos += static_cast<off_t>(total_lookup_size);
        if (version != kFileVersionV7) {
            pos = static_cast<off_t>(RoundUp4K(static_cast<uint64_t>(pos)));
        }
    }

    return Status::OK();
}

std::vector<uint32_t> ClusterStoreReader::cluster_ids() const {
    std::vector<uint32_t> ids;
    ids.reserve(info_.lookup_table.size());
    for (const auto& e : info_.lookup_table) {
        ids.push_back(e.cluster_id);
    }
    return ids;
}

uint32_t ClusterStoreReader::GetNumRecords(uint32_t cluster_id) const {
    auto it = cluster_index_.find(cluster_id);
    if (it == cluster_index_.end()) return 0;
    return info_.lookup_table[it->second].num_records;
}

const float* ClusterStoreReader::GetCentroid(uint32_t cluster_id) const {
    auto it = cluster_index_.find(cluster_id);
    if (it == cluster_index_.end()) return nullptr;
    return info_.lookup_table[it->second].centroid.data();
}

float ClusterStoreReader::GetEpsilon(uint32_t cluster_id) const {
    auto it = cluster_index_.find(cluster_id);
    if (it == cluster_index_.end()) return 0.0f;
    return info_.lookup_table[it->second].epsilon;
}

uint64_t ClusterStoreReader::total_records() const {
    uint64_t total = 0;
    for (const auto& e : info_.lookup_table) {
        total += e.num_records;
    }
    return total;
}

Status ClusterStoreReader::EnsureClusterLoaded(uint32_t cluster_id) {
    if (fd_ < 0) {
        return Status::InvalidArgument("ClusterStoreReader not open");
    }
    if (loaded_clusters_.count(cluster_id)) {
        return Status::OK();
    }

    auto it = cluster_index_.find(cluster_id);
    if (it == cluster_index_.end()) {
        return Status::InvalidArgument(
            "Cluster " + std::to_string(cluster_id) + " not found");
    }

    const auto& entry = info_.lookup_table[it->second];
    ClusterData data;
    const uint64_t block_end = entry.block_offset + entry.block_size;
    uint32_t mini_trailer_size = 0;
    uint32_t block_magic = 0;

    if (!PreadValue(fd_, static_cast<off_t>(block_end - 8), mini_trailer_size)) {
        return Status::IOError("Failed to read mini_trailer_size");
    }
    if (!PreadValue(fd_, static_cast<off_t>(block_end - 4), block_magic)) {
        return Status::IOError("Failed to read block_magic");
    }
    if (block_magic != kBlockMagic) {
        return Status::Corruption("Invalid block magic");
    }

    const uint64_t trailer_start = block_end - mini_trailer_size;
    std::vector<uint8_t> trailer_buf(mini_trailer_size);
    if (!PreadBytes(fd_, static_cast<off_t>(trailer_start),
                    trailer_buf.data(), mini_trailer_size)) {
        return Status::IOError("Failed to read block mini-trailer");
    }

    size_t tpos = 0;
    auto ReadT = [&](auto& val) -> bool {
        if (tpos + sizeof(val) > trailer_buf.size()) return false;
        std::memcpy(&val, trailer_buf.data() + tpos, sizeof(val));
        tpos += sizeof(val);
        return true;
    };

    uint32_t address_format_version = 0;
    uint32_t v9_page_size = 0;
    uint32_t v9_entry_size = 0;
    uint32_t v9_num_entries = 0;
    uint32_t v9_payload_offset = 0;
    uint32_t v9_payload_bytes = 0;
    uint32_t num_blocks = 0;

    if (file_version_ >= kFileVersionV9) {
        if (!ReadT(address_format_version) ||
            !ReadT(v9_page_size) ||
            !ReadT(v9_entry_size) ||
            !ReadT(v9_num_entries) ||
            !ReadT(v9_payload_offset) ||
            !ReadT(v9_payload_bytes)) {
            return Status::Corruption("Mini-trailer: failed to read V9 address table metadata");
        }
        if (address_format_version != kAddressFormatV2) {
            return Status::Corruption("Unsupported V9 address format");
        }
        if (v9_entry_size != sizeof(RawAddressEntryV2)) {
            return Status::Corruption("Unexpected V9 raw address entry size");
        }
        if (v9_num_entries != entry.num_records) {
            return Status::Corruption("V9 raw address entry count mismatch");
        }
        data.address_format = AddressFormat::V2RawTable;
        data.address_layout.page_size = v9_page_size;
    } else {
        if (!ReadT(data.address_layout.page_size) ||
            !ReadT(data.address_layout.bit_width) ||
            !ReadT(data.address_layout.block_granularity) ||
            !ReadT(data.address_layout.fixed_packed_size) ||
            !ReadT(data.address_layout.last_packed_size) ||
            !ReadT(data.address_layout.num_address_blocks)) {
            return Status::Corruption("Mini-trailer: failed to read shared address layout");
        }

        num_blocks = data.address_layout.num_address_blocks;
        if (entry.num_records == 0) {
            if (num_blocks != 0 || data.address_layout.last_packed_size != 0) {
                return Status::Corruption("Empty cluster has invalid address layout");
            }
        } else if (num_blocks == 0) {
            return Status::Corruption("Non-empty cluster has zero address blocks");
        }

        data.address_blocks.resize(num_blocks);
        for (uint32_t i = 0; i < num_blocks; ++i) {
            if (!ReadT(data.address_blocks[i].base_offset)) {
                return Status::Corruption(
                    "Mini-trailer: failed to parse block " + std::to_string(i));
            }
        }
        data.address_format = AddressFormat::V1Packed;
    }

    uint32_t stored_trailer_size = 0;
    uint32_t stored_block_magic = 0;
    if (!ReadT(stored_trailer_size) || !ReadT(stored_block_magic)) {
        return Status::Corruption("Mini-trailer: failed to read trailer footer");
    }
    if (stored_trailer_size != mini_trailer_size || stored_block_magic != kBlockMagic) {
        return Status::Corruption("Mini-trailer footer mismatch");
    }
    if (tpos != trailer_buf.size()) {
        return Status::Corruption("Mini-trailer has trailing bytes");
    }
    if (data.address_format == AddressFormat::V1Packed &&
        num_blocks > 0 && data.address_layout.block_granularity == 0) {
        return Status::Corruption("Invalid address block granularity");
    }

    data.codes_offset = entry.block_offset;
    const uint32_t region1_size = entry.num_fastscan_blocks * fastscan_block_bytes();
    const uint32_t region2_size =
        (file_version_ >= kFileVersionV11)
            ? (v9_payload_offset >= region1_size ? v9_payload_offset - region1_size : 0)
            : entry.num_records * exrabitq_entry_size();
    data.codes_length = region1_size + region2_size;

    if (data.codes_length > 0) {
        data.codes_buffer.resize(data.codes_length);
        if (!PreadBytes(fd_, static_cast<off_t>(data.codes_offset),
                        data.codes_buffer.data(), data.codes_length)) {
            return Status::IOError("Failed to read codes buffer");
        }
    }

    const uint64_t default_payload_offset = entry.block_offset + data.codes_length;
    if (default_payload_offset > block_end || data.codes_length > entry.block_size) {
        return Status::Corruption("Cluster block shorter than RaBitQ code region");
    }

    uint64_t expected_payload_bytes = 0;
    uint64_t address_payload_offset = default_payload_offset;
    if (data.address_format == AddressFormat::V2RawTable) {
        address_payload_offset = entry.block_offset + static_cast<uint64_t>(v9_payload_offset);
        expected_payload_bytes = v9_payload_bytes;
    } else if (num_blocks > 0) {
        expected_payload_bytes =
            static_cast<uint64_t>(num_blocks - 1) * data.address_layout.fixed_packed_size +
            data.address_layout.last_packed_size;
    }
    const uint64_t payload_and_trailer = entry.block_size - data.codes_length;
    if (payload_and_trailer < mini_trailer_size) {
        return Status::Corruption("Cluster block shorter than trailer");
    }
    const uint64_t actual_payload_bytes = payload_and_trailer - mini_trailer_size;
    if (actual_payload_bytes != expected_payload_bytes) {
        return Status::Corruption("Address payload size mismatch");
    }

    if (data.address_format == AddressFormat::V2RawTable) {
        data.raw_addresses_v2.resize(entry.num_records);
        if (v9_payload_bytes > 0 &&
            !PreadBytes(fd_, static_cast<off_t>(address_payload_offset),
                        data.raw_addresses_v2.data(), v9_payload_bytes)) {
            return Status::IOError("Failed to read raw address table");
        }
        EncodedAddressColumn raw_column;
        raw_column.format = AddressFormat::V2RawTable;
        raw_column.layout.page_size = v9_page_size;
        raw_column.total_records = entry.num_records;
        raw_column.raw_entries = data.raw_addresses_v2;
        VDB_RETURN_IF_ERROR(AddressColumn::DecodeRawTableV2(
            raw_column, data.decoded_addresses));
    } else {
        uint64_t addr_read_offset = address_payload_offset;
        for (uint32_t i = 0; i < num_blocks; ++i) {
            auto& block = data.address_blocks[i];
            const uint32_t packed_size =
                AddressColumn::BlockPackedSize(data.address_layout, i);
            block.packed.resize(packed_size);
            if (packed_size == 0) continue;

            if (!PreadBytes(fd_, static_cast<off_t>(addr_read_offset),
                            block.packed.data(), packed_size)) {
                return Status::IOError("Failed to read address block packed data");
            }
            addr_read_offset += packed_size;
        }

        VDB_RETURN_IF_ERROR(AddressColumn::DecodeBatchBlocks(
            data.address_layout, data.address_blocks, entry.num_records,
            data.decoded_addresses));
    }

    loaded_clusters_[cluster_id] = std::move(data);
    return Status::OK();
}

Status ClusterStoreReader::UnloadCluster(uint32_t cluster_id) {
    if (resident_preload_ready_) {
        return Status::OK();
    }
    loaded_clusters_.erase(cluster_id);
    return Status::OK();
}

std::optional<query::ClusterBlockLocation>
ClusterStoreReader::GetBlockLocation(uint32_t cluster_id) const {
    auto it = cluster_index_.find(cluster_id);
    if (it == cluster_index_.end()) return std::nullopt;
    const auto& entry = info_.lookup_table[it->second];
    return query::ClusterBlockLocation{entry.block_offset, entry.block_size,
                                       entry.num_records};
}

Status ClusterStoreReader::ParseClusterBlockView(
    uint32_t cluster_id,
    const uint8_t* block_ptr,
    uint64_t block_size,
    query::ParsedCluster& out) const {
    auto idx_it = cluster_index_.find(cluster_id);
    if (idx_it == cluster_index_.end()) {
        return Status::InvalidArgument(
            "Cluster " + std::to_string(cluster_id) + " not found");
    }
    const auto& entry = info_.lookup_table[idx_it->second];

    if (block_ptr == nullptr) {
        return Status::InvalidArgument("block_ptr is null");
    }
    if (block_size != entry.block_size) {
        return Status::InvalidArgument("block_size mismatch");
    }
    if (block_size < 8) {
        return Status::Corruption("Block too small for trailer footer");
    }

    uint32_t mini_trailer_size = 0;
    uint32_t block_magic = 0;
    std::memcpy(&mini_trailer_size, block_ptr + block_size - 8, sizeof(uint32_t));
    std::memcpy(&block_magic, block_ptr + block_size - 4, sizeof(uint32_t));
    if (block_magic != kBlockMagic) {
        return Status::Corruption("Invalid block magic");
    }
    if (mini_trailer_size > block_size) {
        return Status::Corruption("Mini-trailer size exceeds block");
    }

    const uint8_t* trailer_ptr = block_ptr + block_size - mini_trailer_size;
    size_t tpos = 0;
    auto ReadT = [&](auto& val) -> bool {
        if (tpos + sizeof(val) > mini_trailer_size) return false;
        std::memcpy(&val, trailer_ptr + tpos, sizeof(val));
        tpos += sizeof(val);
        return true;
    };

    AddressColumnLayout address_layout;
    std::vector<AddressBlock> address_blocks;
    const RawAddressEntryV2* raw_addresses = nullptr;
    uint32_t address_page_size = 0;
    bool addresses_are_raw_v2 = false;
    uint32_t num_blocks = 0;
    uint32_t v9_payload_offset = 0;
    uint32_t v9_payload_bytes = 0;

    if (file_version_ >= kFileVersionV9) {
        uint32_t address_format_version = 0;
        uint32_t address_entry_size = 0;
        uint32_t num_entries = 0;
        if (!ReadT(address_format_version) ||
            !ReadT(address_page_size) ||
            !ReadT(address_entry_size) ||
            !ReadT(num_entries) ||
            !ReadT(v9_payload_offset) ||
            !ReadT(v9_payload_bytes)) {
            return Status::Corruption("Mini-trailer: failed to read V9 address metadata");
        }
        if (address_format_version != kAddressFormatV2) {
            return Status::Corruption("Unsupported V9 address format");
        }
        if (address_entry_size != sizeof(RawAddressEntryV2)) {
            return Status::Corruption("Unexpected V9 raw address entry size");
        }
        if (num_entries != entry.num_records) {
            return Status::Corruption("V9 raw address entry count mismatch");
        }
        addresses_are_raw_v2 = true;
    } else {
        if (!ReadT(address_layout.page_size) ||
            !ReadT(address_layout.bit_width) ||
            !ReadT(address_layout.block_granularity) ||
            !ReadT(address_layout.fixed_packed_size) ||
            !ReadT(address_layout.last_packed_size) ||
            !ReadT(address_layout.num_address_blocks)) {
            return Status::Corruption("Mini-trailer: failed to read shared address layout");
        }

        num_blocks = address_layout.num_address_blocks;
        if (entry.num_records == 0) {
            if (num_blocks != 0 || address_layout.last_packed_size != 0) {
                return Status::Corruption("Empty cluster has invalid address layout");
            }
        } else if (num_blocks == 0) {
            return Status::Corruption("Non-empty cluster has zero address blocks");
        }

        address_blocks.resize(num_blocks);
        for (uint32_t i = 0; i < num_blocks; ++i) {
            if (!ReadT(address_blocks[i].base_offset)) {
                return Status::Corruption(
                    "Mini-trailer: failed to parse block " + std::to_string(i));
            }
        }
    }

    uint32_t stored_trailer_size = 0;
    uint32_t stored_block_magic = 0;
    if (!ReadT(stored_trailer_size) || !ReadT(stored_block_magic)) {
        return Status::Corruption("Mini-trailer: failed to read trailer footer");
    }
    if (stored_trailer_size != mini_trailer_size || stored_block_magic != kBlockMagic) {
        return Status::Corruption("Mini-trailer footer mismatch");
    }
    if (tpos != mini_trailer_size) {
        return Status::Corruption("Mini-trailer has trailing bytes");
    }
    if (!addresses_are_raw_v2 &&
        num_blocks > 0 && address_layout.block_granularity == 0) {
        return Status::Corruption("Invalid address block granularity");
    }

    const uint32_t fb_size = fastscan_block_bytes();
    const uint32_t region1_size = entry.num_fastscan_blocks * fb_size;
    const uint32_t ex_entry_size = exrabitq_entry_size();
    const uint32_t region2_size =
        (file_version_ >= kFileVersionV11)
            ? (v9_payload_offset >= region1_size ? v9_payload_offset - region1_size : 0)
            : entry.num_records * ex_entry_size;
    const uint32_t codes_length = region1_size + region2_size;
    if (codes_length > block_size) {
        return Status::Corruption("Cluster block shorter than code regions");
    }

    uint64_t expected_payload_bytes = 0;
    if (addresses_are_raw_v2) {
        expected_payload_bytes = v9_payload_bytes;
    } else if (num_blocks > 0) {
        expected_payload_bytes =
            static_cast<uint64_t>(num_blocks - 1) * address_layout.fixed_packed_size +
            address_layout.last_packed_size;
    }
    const uint64_t payload_and_trailer = block_size - codes_length;
    if (payload_and_trailer < mini_trailer_size) {
        return Status::Corruption("Cluster block shorter than trailer");
    }
    const uint64_t actual_payload_bytes = payload_and_trailer - mini_trailer_size;
    if (actual_payload_bytes != expected_payload_bytes) {
        return Status::Corruption("Address payload size mismatch");
    }

    std::vector<AddressEntry> decoded;
    if (addresses_are_raw_v2) {
        if (static_cast<uint64_t>(v9_payload_offset) + v9_payload_bytes >
            block_size - mini_trailer_size) {
            return Status::Corruption("V9 raw address payload exceeds block");
        }
        raw_addresses = reinterpret_cast<const RawAddressEntryV2*>(
            block_ptr + v9_payload_offset);
        decoded.resize(entry.num_records);
        for (uint32_t i = 0; i < entry.num_records; ++i) {
            decoded[i] = AddressColumn::DecodeRawEntryV2(raw_addresses[i],
                                                         address_page_size);
        }
    } else {
        uint64_t addr_offset = codes_length;
        for (uint32_t i = 0; i < num_blocks; ++i) {
            auto& block = address_blocks[i];
            const uint32_t packed_size =
                AddressColumn::BlockPackedSize(address_layout, i);
            if (packed_size > 0) {
                block.packed.assign(block_ptr + addr_offset,
                                    block_ptr + addr_offset + packed_size);
            }
            addr_offset += packed_size;
        }

        VDB_RETURN_IF_ERROR(AddressColumn::DecodeBatchBlocks(
            address_layout, address_blocks, entry.num_records, decoded));
    }

    out.fastscan_blocks = block_ptr;
    out.fastscan_block_size = fb_size;
    out.num_fastscan_blocks = entry.num_fastscan_blocks;
    out.exrabitq_entries =
        (ex_entry_size > 0) ? block_ptr + region1_size : nullptr;
    out.exrabitq_entry_size = ex_entry_size;
    out.exrabitq_sign_bytes = exrabitq_sign_bytes();
    out.exrabitq_sign_packed = (file_version_ >= kFileVersionV10);
    out.exrabitq_storage_version = file_version_;
    const bool variable_ex_blocks =
        file_version_ >= kFileVersionV15 &&
        UsesVariableOfficialExDataLayout(info_.rabitq_config.effective_exdata_layout());
    out.exrabitq_batch_blocks =
        (file_version_ >= kFileVersionV11 &&
         info_.rabitq_config.stage2_payload_bits() > 0)
            ? block_ptr + region1_size
            : nullptr;
    out.exrabitq_batch_block_size = variable_ex_blocks ? 0 : exrabitq_batch_block_size();
    out.exrabitq_batch_region_bytes = region2_size;
    out.exrabitq_variable_batch_blocks = variable_ex_blocks;
    out.exrabitq_batch_block_offsets = nullptr;
    if (variable_ex_blocks) {
        if (region2_size < 12) {
            return Status::Corruption("Variable ExRaBitQ region is too small");
        }
        uint32_t magic = 0;
        uint32_t stored_batch_blocks = 0;
        std::memcpy(&magic, out.exrabitq_batch_blocks, sizeof(uint32_t));
        std::memcpy(&stored_batch_blocks, out.exrabitq_batch_blocks + 4, sizeof(uint32_t));
        if (magic != kVariableExRegionMagic ||
            stored_batch_blocks != exrabitq_num_batch_blocks(entry.num_records)) {
            return Status::Corruption("Variable ExRaBitQ region header mismatch");
        }
        const uint64_t offset_table_bytes =
            8ull + static_cast<uint64_t>(stored_batch_blocks + 1u) * sizeof(uint32_t);
        if (offset_table_bytes > region2_size) {
            return Status::Corruption("Variable ExRaBitQ offset table exceeds region");
        }
        out.exrabitq_batch_block_offsets =
            reinterpret_cast<const uint32_t*>(out.exrabitq_batch_blocks + 8);
        uint32_t first_offset = 0;
        uint32_t last_offset = 0;
        std::memcpy(&first_offset, out.exrabitq_batch_blocks + 8, sizeof(uint32_t));
        std::memcpy(&last_offset,
                    out.exrabitq_batch_blocks + 8 +
                        stored_batch_blocks * sizeof(uint32_t),
                    sizeof(uint32_t));
        if (first_offset != offset_table_bytes || last_offset != region2_size) {
            return Status::Corruption("Variable ExRaBitQ offsets have invalid bounds");
        }
    }
    out.exrabitq_batch_size = exrabitq_batch_size();
    out.exrabitq_dim_block = exrabitq_dim_block();
    out.exrabitq_num_dim_blocks = exrabitq_dim_block_count();
    out.exrabitq_num_batch_blocks = exrabitq_num_batch_blocks(entry.num_records);
    out.exrabitq_abs_bytes_per_lane_dim_block =
        exrabitq_abs_bytes_per_lane_dim_block();
    out.exrabitq_magnitude_bits = info_.rabitq_config.stage2_payload_bits();
    out.exrabitq_magnitude_packed = exrabitq_magnitude_packed();
    out.rabitq_total_bits = info_.rabitq_config.effective_total_bits();
    out.rabitq_ex_bits = info_.rabitq_config.stage2_payload_bits();
    out.rabitq_estimator_mode = info_.rabitq_config.estimator_mode;
    out.rabitq_exdata_layout = info_.rabitq_config.effective_exdata_layout();
    out.num_records = entry.num_records;
    out.epsilon = entry.epsilon;
    out.raw_addresses = raw_addresses;
    out.address_page_size = address_page_size;
    out.addresses_are_raw_v2 = addresses_are_raw_v2;
    out.decoded_addresses = std::move(decoded);
    out.decoded_addresses_data =
        out.decoded_addresses.empty() ? nullptr : out.decoded_addresses.data();
    out.decoded_address_count =
        static_cast<uint32_t>(out.decoded_addresses.size());
    out.fastscan_region_offset = 0;
    out.exrabitq_region_offset = region1_size;
    out.code_region_bytes = codes_length;
    out.address_payload_offset =
        addresses_are_raw_v2 ? v9_payload_offset : codes_length;
    out.address_payload_bytes = expected_payload_bytes;
    out.mini_trailer_offset = block_size - mini_trailer_size;
    out.mini_trailer_size = mini_trailer_size;
    out.codes_start = out.fastscan_blocks;
    out.code_entry_size = 0;
    return Status::OK();
}

Status ClusterStoreReader::ParseClusterBlock(
    uint32_t cluster_id,
    query::AlignedBufPtr block_buf,
    uint64_t block_size,
    query::ParsedCluster& out) {
    VDB_RETURN_IF_ERROR(
        ParseClusterBlockView(cluster_id, block_buf.get(), block_size, out));
    out.block_buf = std::move(block_buf);
    return Status::OK();
}

Status ClusterStoreReader::PreloadAllClusters() {
    if (fd_ < 0) {
        return Status::InvalidArgument("ClusterStoreReader not open");
    }
    if (resident_preload_ready_) {
        return Status::OK();
    }

    auto t0 = std::chrono::steady_clock::now();
    const off_t file_size = ::lseek(fd_, 0, SEEK_END);
    if (file_size < 0) {
        return Status::IOError("Failed to determine .clu file size");
    }

	    if (UseCompactResidentPreload()) {
	        const bool use_code_slab = UseCompactCodeSlabResidentPreload();
	        const bool precompute_stage1_envelope = UseStage1EnvelopePrecompute();
	        std::map<uint32_t, ResidentClusterView> resident_clusters;
	        std::map<uint32_t, query::ParsedCluster> resident_parsed_clusters;
	        uint64_t resident_cluster_mem_bytes = 0;
        uint64_t resident_parallel_view_bytes = 0;
        uint64_t resident_stage1_envelope_bytes = 0;
        uint64_t resident_code_storage_bytes = 0;
        uint64_t resident_decoded_address_bytes = 0;
        uint64_t resident_preload_bytes = 0;
        double parallel_build_ms = 0.0;

        std::vector<const ClusterStoreWriter::ClusterLookupEntry*> ordered;
        ordered.reserve(info_.lookup_table.size());
        for (const auto& entry : info_.lookup_table) {
            if (entry.block_offset + entry.block_size >
                static_cast<uint64_t>(file_size)) {
                return Status::Corruption("Cluster block exceeds .clu file size");
            }
            ordered.push_back(&entry);
        }
        std::sort(ordered.begin(), ordered.end(),
	                  [](const auto* a, const auto* b) {
	                      return a->block_offset < b->block_offset;
	                  });

        constexpr size_t kBatchSize = 16;
        constexpr uint64_t kMaxSpanWasteRatio = 4;
        constexpr uint64_t kCodeSlabClusterAlign = 64;
        std::map<uint32_t, uint64_t> code_slab_offsets;
        uint64_t code_slab_bytes = 0;
        auto read_code_region_bytes =
            [&](const ClusterStoreWriter::ClusterLookupEntry& entry,
                uint32_t* code_region_bytes,
                uint64_t* io_bytes) -> Status {
            if (entry.block_size < 8) {
                return Status::Corruption("Block too small for trailer footer");
            }
            const uint64_t block_end = entry.block_offset + entry.block_size;
            uint32_t mini_trailer_size = 0;
            uint32_t block_magic = 0;
            if (!PreadValue(fd_, static_cast<off_t>(block_end - 8),
                            mini_trailer_size)) {
                return Status::IOError("Failed to read mini_trailer_size");
            }
            if (!PreadValue(fd_, static_cast<off_t>(block_end - 4),
                            block_magic)) {
                return Status::IOError("Failed to read block_magic");
            }
            if (block_magic != kBlockMagic) {
                return Status::Corruption("Invalid block magic");
            }
            if (mini_trailer_size > entry.block_size) {
                return Status::Corruption("Mini-trailer size exceeds block");
            }

            const uint64_t trailer_start = block_end - mini_trailer_size;
            std::vector<uint8_t> trailer_buf(mini_trailer_size);
            if (!PreadBytes(fd_, static_cast<off_t>(trailer_start),
                            trailer_buf.data(), mini_trailer_size)) {
                return Status::IOError("Failed to read block mini-trailer");
            }
            if (io_bytes != nullptr) {
                *io_bytes += 8ull + mini_trailer_size;
            }

            size_t tpos = 0;
            auto ReadT = [&](auto& val) -> bool {
                if (tpos + sizeof(val) > trailer_buf.size()) return false;
                std::memcpy(&val, trailer_buf.data() + tpos, sizeof(val));
                tpos += sizeof(val);
                return true;
            };

            AddressColumnLayout address_layout;
            uint32_t num_blocks = 0;
            uint32_t v9_payload_offset = 0;
            uint32_t v9_payload_bytes = 0;
            uint64_t expected_payload_bytes = 0;

            if (file_version_ >= kFileVersionV9) {
                uint32_t address_format_version = 0;
                uint32_t address_page_size = 0;
                uint32_t address_entry_size = 0;
                uint32_t num_entries = 0;
                if (!ReadT(address_format_version) ||
                    !ReadT(address_page_size) ||
                    !ReadT(address_entry_size) ||
                    !ReadT(num_entries) ||
                    !ReadT(v9_payload_offset) ||
                    !ReadT(v9_payload_bytes)) {
                    return Status::Corruption(
                        "Mini-trailer: failed to read V9 address metadata");
                }
                (void)address_page_size;
                if (address_format_version != kAddressFormatV2) {
                    return Status::Corruption("Unsupported V9 address format");
                }
                if (address_entry_size != sizeof(RawAddressEntryV2)) {
                    return Status::Corruption(
                        "Unexpected V9 raw address entry size");
                }
                if (num_entries != entry.num_records) {
                    return Status::Corruption("V9 raw address entry count mismatch");
                }
                expected_payload_bytes = v9_payload_bytes;
            } else {
                if (!ReadT(address_layout.page_size) ||
                    !ReadT(address_layout.bit_width) ||
                    !ReadT(address_layout.block_granularity) ||
                    !ReadT(address_layout.fixed_packed_size) ||
                    !ReadT(address_layout.last_packed_size) ||
                    !ReadT(address_layout.num_address_blocks)) {
                    return Status::Corruption(
                        "Mini-trailer: failed to read shared address layout");
                }

                num_blocks = address_layout.num_address_blocks;
                if (entry.num_records == 0) {
                    if (num_blocks != 0 || address_layout.last_packed_size != 0) {
                        return Status::Corruption(
                            "Empty cluster has invalid address layout");
                    }
                } else if (num_blocks == 0) {
                    return Status::Corruption("Non-empty cluster has zero address blocks");
                }
                for (uint32_t block_idx = 0; block_idx < num_blocks; ++block_idx) {
                    uint32_t base_offset = 0;
                    if (!ReadT(base_offset)) {
                        return Status::Corruption(
                            "Mini-trailer: failed to parse address block");
                    }
                }
                if (num_blocks > 0 && address_layout.block_granularity == 0) {
                    return Status::Corruption("Invalid address block granularity");
                }
                if (num_blocks > 0) {
                    expected_payload_bytes =
                        static_cast<uint64_t>(num_blocks - 1) *
                            address_layout.fixed_packed_size +
                        address_layout.last_packed_size;
                }
            }

            uint32_t stored_trailer_size = 0;
            uint32_t stored_block_magic = 0;
            if (!ReadT(stored_trailer_size) || !ReadT(stored_block_magic)) {
                return Status::Corruption(
                    "Mini-trailer: failed to read trailer footer");
            }
            if (stored_trailer_size != mini_trailer_size ||
                stored_block_magic != kBlockMagic) {
                return Status::Corruption("Mini-trailer footer mismatch");
            }
            if (tpos != trailer_buf.size()) {
                return Status::Corruption("Mini-trailer has trailing bytes");
            }

            const uint32_t region1_size =
                entry.num_fastscan_blocks * fastscan_block_bytes();
            const uint32_t region2_size =
                (file_version_ >= kFileVersionV11)
                    ? (v9_payload_offset >= region1_size
                           ? v9_payload_offset - region1_size
                           : 0)
                    : entry.num_records * exrabitq_entry_size();
            const uint32_t codes_length = region1_size + region2_size;
            if (codes_length > entry.block_size) {
                return Status::Corruption(
                    "Cluster block shorter than code regions");
            }
            const uint64_t payload_and_trailer = entry.block_size - codes_length;
            if (payload_and_trailer < mini_trailer_size) {
                return Status::Corruption("Cluster block shorter than trailer");
            }
            const uint64_t actual_payload_bytes =
                payload_and_trailer - mini_trailer_size;
            if (actual_payload_bytes != expected_payload_bytes) {
                return Status::Corruption("Address payload size mismatch");
            }
            *code_region_bytes = codes_length;
            return Status::OK();
        };
        if (use_code_slab) {
            uint64_t trailer_prepass_bytes = 0;
            for (const auto* entry : ordered) {
                uint32_t code_region_bytes = 0;
                VDB_RETURN_IF_ERROR(read_code_region_bytes(
                    *entry, &code_region_bytes, &trailer_prepass_bytes));
                code_slab_bytes = AlignUp64(code_slab_bytes, kCodeSlabClusterAlign);
                code_slab_offsets[entry->cluster_id] = code_slab_bytes;
                code_slab_bytes += code_region_bytes;
            }
            resident_preload_bytes += trailer_prepass_bytes;
            VDB_RETURN_IF_ERROR(AllocateResidentCodeSlabBuffer(code_slab_bytes));
        } else {
            ReleaseResidentCodeSlabBuffer();
        }

	        size_t i = 0;
	        while (i < ordered.size()) {
            size_t batch_end = i + 1;
            uint64_t span_begin = ordered[i]->block_offset;
            uint64_t span_end = ordered[i]->block_offset + ordered[i]->block_size;
            uint64_t used_bytes = ordered[i]->block_size;

            while (batch_end < ordered.size() && batch_end - i < kBatchSize) {
                const auto* next = ordered[batch_end];
                const uint64_t next_end = next->block_offset + next->block_size;
                const uint64_t candidate_span_end = std::max(span_end, next_end);
                const uint64_t candidate_used = used_bytes + next->block_size;
                const uint64_t candidate_span = candidate_span_end - span_begin;
                if (candidate_span > candidate_used * kMaxSpanWasteRatio) {
                    break;
                }
                span_end = candidate_span_end;
                used_bytes = candidate_used;
                ++batch_end;
            }

            const uint64_t span_bytes = span_end - span_begin;
            std::vector<uint8_t> batch_buffer(static_cast<size_t>(span_bytes));
            if (span_bytes > 0 &&
                !PreadBytes(fd_, static_cast<off_t>(span_begin),
                            batch_buffer.data(), batch_buffer.size())) {
                return Status::IOError("Failed to batch-read resident cluster blocks");
            }
            resident_preload_bytes += span_bytes;

            for (size_t j = i; j < batch_end; ++j) {
                const auto* entry = ordered[j];
                const uint64_t relative = entry->block_offset - span_begin;
                if (relative + entry->block_size > span_bytes) {
                    return Status::Corruption("Cluster block exceeds preload batch buffer");
                }
                query::ParsedCluster parsed;
                const uint8_t* block_ptr =
                    batch_buffer.data() + static_cast<size_t>(relative);
                VDB_RETURN_IF_ERROR(ParseClusterBlockView(
                    entry->cluster_id, block_ptr, entry->block_size, parsed));
	                ResidentClusterView view;
	                if (use_code_slab) {
	                    CopyParsedMetadataToResident(parsed, &view);
	                    const uint64_t slab_offset =
	                        code_slab_offsets[entry->cluster_id];
	                    uint8_t* slab_base = ResidentCodeSlabData();
	                    if (parsed.code_region_bytes > 0) {
	                        if (slab_base == nullptr ||
	                            slab_offset + parsed.code_region_bytes >
	                                code_slab_bytes) {
	                            return Status::Corruption(
	                                "Resident code slab offset exceeds allocation");
	                        }
	                        std::memcpy(slab_base + slab_offset,
	                                    parsed.fastscan_blocks,
	                                    parsed.code_region_bytes);
	                    }
	                    view.code_storage_bytes = parsed.code_region_bytes;
	                    RelocateResidentCodePointersToBase(
	                        parsed,
	                        slab_base == nullptr ? nullptr : slab_base + slab_offset,
	                        &view);
	                    view.raw_addresses = nullptr;
	                    view.addresses_are_raw_v2 = false;
	                    view.raw_address_bytes = 0;
	                    view.decoded_addresses = std::move(parsed.decoded_addresses);
	                    view.decoded_address_bytes =
	                        static_cast<uint64_t>(view.decoded_addresses.size()) *
	                        sizeof(AddressEntry);
	                } else {
	                    view = MakeCompactResidentView(parsed);
	                }
	                uint64_t stage1_envelope_bytes = 0;
	                if (precompute_stage1_envelope) {
	                    BuildResidentStage1EnvelopeSummary(parsed, info_.dim, &view);
	                    stage1_envelope_bytes = ResidentStage1EnvelopeBytes(view);
	                    resident_stage1_envelope_bytes += stage1_envelope_bytes;
	                }
	                auto pb0 = std::chrono::steady_clock::now();
	                BuildResidentParallelStage2View(parsed, &view);
                MAdviseHugePageVector(view.exrabitq_parallel_abs_blocks_storage);
                MAdviseHugePageVector(view.exrabitq_parallel_sign_words_storage);
                parallel_build_ms += std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - pb0).count();
                const uint64_t parallel_bytes =
                    static_cast<uint64_t>(view.exrabitq_parallel_abs_blocks_storage.size()) +
                    static_cast<uint64_t>(view.exrabitq_parallel_sign_words_storage.size()) *
                        sizeof(uint16_t);
                resident_parallel_view_bytes += parallel_bytes;
                resident_code_storage_bytes += view.code_storage_bytes;
                resident_decoded_address_bytes += view.decoded_address_bytes;

                auto inserted =
                    resident_clusters.emplace(entry->cluster_id, std::move(view));
                if (!inserted.second) {
                    return Status::Corruption("Duplicate resident cluster");
                }
	                if (!use_code_slab) {
	                    resident_cluster_mem_bytes +=
	                        inserted.first->second.code_storage_bytes;
	                }
	                resident_cluster_mem_bytes +=
	                    inserted.first->second.decoded_address_bytes;
                resident_cluster_mem_bytes += parallel_bytes;
                resident_cluster_mem_bytes += stage1_envelope_bytes;
                resident_parsed_clusters.emplace(
                    entry->cluster_id, inserted.first->second.ToParsedCluster());
            }
            i = batch_end;
        }

        if (use_code_slab) {
            MAdviseHugePageRange(ResidentCodeSlabData(),
                                  static_cast<size_t>(code_slab_bytes));
        }

        resident_file_buffer_.clear();
	        resident_file_buffer_.shrink_to_fit();
	        ReleaseResidentMmapFileBuffer();
	        resident_clusters_ = std::move(resident_clusters);
        resident_parsed_clusters_ = std::move(resident_parsed_clusters);
        resident_preload_ready_ = true;
        resident_preload_bytes_ = resident_preload_bytes;
        resident_cluster_mem_bytes_ = resident_cluster_mem_bytes;
        resident_file_size_bytes_ = static_cast<uint64_t>(file_size);
	        resident_file_buffer_bytes_ = 0;
	        resident_code_storage_bytes_ = resident_code_storage_bytes;
        resident_decoded_address_bytes_ = resident_decoded_address_bytes;
        resident_raw_address_bytes_ = 0;
        resident_parsed_address_duplicate_bytes_ = 0;
        resident_preload_batch_size_ = 16;
	        if (use_code_slab) {
	            resident_cluster_mem_bytes_ += code_slab_bytes;
	            resident_preload_mode_ = "compact_code_mmap_2mb";
	        } else {
	            resident_preload_mode_ = "compact_batched";
	        }
        resident_preload_time_ms_ = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - t0).count();
        resident_parallel_view_bytes_ = resident_parallel_view_bytes;
        resident_parallel_view_build_ms_ = parallel_build_ms;
        resident_stage1_envelope_bytes_ = resident_stage1_envelope_bytes;
        return Status::OK();
    }

    const bool use_aligned_mmap = UseAlignedMmapResidentFile();
    ReleaseResidentCodeSlabBuffer();
    if (use_aligned_mmap) {
	        resident_file_buffer_.clear();
	        resident_file_buffer_.shrink_to_fit();
	        VDB_RETURN_IF_ERROR(
	            AllocateResidentMmapFileBuffer(static_cast<uint64_t>(file_size)));
	    } else {
	        ReleaseResidentMmapFileBuffer();
	        resident_file_buffer_.resize(static_cast<size_t>(file_size));
	    }
	    uint8_t* resident_file_data = ResidentFileBufferData();
	    const uint64_t resident_file_size = ResidentFileBufferSize();
	    if (file_size > 0 &&
	        !PreadBytes(fd_, 0, resident_file_data,
	                    static_cast<size_t>(resident_file_size))) {
	        resident_file_buffer_.clear();
	        ReleaseResidentMmapFileBuffer();
	        return Status::IOError("Failed to preload .clu file");
	    }
	    MAdviseHugePageRange(resident_file_data, static_cast<size_t>(resident_file_size));

    std::map<uint32_t, ResidentClusterView> resident_clusters;
    std::map<uint32_t, query::ParsedCluster> resident_parsed_clusters;
    uint64_t resident_cluster_mem_bytes = 0;
    uint64_t resident_parallel_view_bytes = 0;
    uint64_t resident_stage1_envelope_bytes = 0;
    uint64_t resident_decoded_address_bytes = 0;
    uint64_t resident_raw_address_bytes = 0;
    const bool precompute_stage1_envelope = UseStage1EnvelopePrecompute();
    auto parallel_build_start = std::chrono::steady_clock::now();
    for (const auto& entry : info_.lookup_table) {
	        if (entry.block_offset + entry.block_size > resident_file_size) {
	            resident_file_buffer_.clear();
	            ReleaseResidentMmapFileBuffer();
	            return Status::Corruption("Cluster block exceeds resident .clu buffer");
	        }

	        query::ParsedCluster parsed;
	        const uint8_t* block_ptr =
	            resident_file_data + static_cast<size_t>(entry.block_offset);
        VDB_RETURN_IF_ERROR(
            ParseClusterBlockView(entry.cluster_id, block_ptr, entry.block_size, parsed));

        ResidentClusterView view;
        view.fastscan_blocks = parsed.fastscan_blocks;
        view.fastscan_block_size = parsed.fastscan_block_size;
        view.num_fastscan_blocks = parsed.num_fastscan_blocks;
        view.exrabitq_entries = parsed.exrabitq_entries;
        view.exrabitq_entry_size = parsed.exrabitq_entry_size;
        view.exrabitq_sign_bytes = parsed.exrabitq_sign_bytes;
        view.exrabitq_sign_packed = parsed.exrabitq_sign_packed;
        view.exrabitq_storage_version = parsed.exrabitq_storage_version;
        view.exrabitq_batch_blocks = parsed.exrabitq_batch_blocks;
        view.exrabitq_batch_block_size = parsed.exrabitq_batch_block_size;
        view.exrabitq_batch_block_offsets = parsed.exrabitq_batch_block_offsets;
        view.exrabitq_batch_region_bytes = parsed.exrabitq_batch_region_bytes;
        view.exrabitq_variable_batch_blocks = parsed.exrabitq_variable_batch_blocks;
        view.exrabitq_batch_size = parsed.exrabitq_batch_size;
        view.exrabitq_dim_block = parsed.exrabitq_dim_block;
        view.exrabitq_num_dim_blocks = parsed.exrabitq_num_dim_blocks;
        view.exrabitq_num_batch_blocks = parsed.exrabitq_num_batch_blocks;
        view.exrabitq_abs_bytes_per_lane_dim_block =
            parsed.exrabitq_abs_bytes_per_lane_dim_block;
        view.exrabitq_magnitude_bits = parsed.exrabitq_magnitude_bits;
        view.exrabitq_magnitude_packed = parsed.exrabitq_magnitude_packed;
        view.rabitq_total_bits = parsed.rabitq_total_bits;
        view.rabitq_ex_bits = parsed.rabitq_ex_bits;
        view.rabitq_estimator_mode = parsed.rabitq_estimator_mode;
        view.rabitq_exdata_layout = parsed.rabitq_exdata_layout;
        view.num_records = parsed.num_records;
        view.epsilon = parsed.epsilon;
        view.raw_addresses = parsed.raw_addresses;
        view.address_page_size = parsed.address_page_size;
        view.addresses_are_raw_v2 = parsed.addresses_are_raw_v2;
        view.decoded_addresses = parsed.decoded_addresses;
        view.decoded_address_bytes =
            static_cast<uint64_t>(view.decoded_addresses.size()) * sizeof(AddressEntry);
        view.raw_address_bytes = parsed.address_payload_bytes;
        uint64_t stage1_envelope_bytes = 0;
        if (precompute_stage1_envelope) {
            BuildResidentStage1EnvelopeSummary(parsed, info_.dim, &view);
            stage1_envelope_bytes = ResidentStage1EnvelopeBytes(view);
            resident_stage1_envelope_bytes += stage1_envelope_bytes;
        }
        BuildResidentParallelStage2View(parsed, &view);
        MAdviseHugePageVector(view.exrabitq_parallel_abs_blocks_storage);
        MAdviseHugePageVector(view.exrabitq_parallel_sign_words_storage);
        resident_parallel_view_bytes +=
            static_cast<uint64_t>(view.exrabitq_parallel_abs_blocks_storage.size());
        resident_parallel_view_bytes +=
            static_cast<uint64_t>(view.exrabitq_parallel_sign_words_storage.size()) *
            sizeof(uint16_t);
        resident_decoded_address_bytes +=
            static_cast<uint64_t>(view.decoded_addresses.size()) * sizeof(AddressEntry);
        resident_raw_address_bytes += parsed.address_payload_bytes;
        auto inserted = resident_clusters.emplace(entry.cluster_id, std::move(view));
        resident_cluster_mem_bytes +=
            static_cast<uint64_t>(parsed.decoded_addresses.size()) *
            sizeof(AddressEntry);
        resident_cluster_mem_bytes +=
            static_cast<uint64_t>(inserted.first->second.exrabitq_parallel_abs_blocks_storage.size());
        resident_cluster_mem_bytes +=
            static_cast<uint64_t>(inserted.first->second.exrabitq_parallel_sign_words_storage.size()) *
            sizeof(uint16_t);
        resident_cluster_mem_bytes += stage1_envelope_bytes;
        resident_parsed_clusters.emplace(
            entry.cluster_id, inserted.first->second.ToParsedCluster());
    }

    resident_clusters_ = std::move(resident_clusters);
    resident_parsed_clusters_ = std::move(resident_parsed_clusters);
    resident_preload_ready_ = true;
	    resident_preload_bytes_ = resident_file_size;
	    resident_cluster_mem_bytes_ =
	        resident_cluster_mem_bytes + resident_file_size;
	    resident_file_size_bytes_ = static_cast<uint64_t>(file_size);
	    resident_file_buffer_bytes_ = resident_file_size;
    resident_code_storage_bytes_ = 0;
    resident_decoded_address_bytes_ = resident_decoded_address_bytes;
    resident_raw_address_bytes_ = resident_raw_address_bytes;
    resident_parsed_address_duplicate_bytes_ = 0;
    resident_preload_batch_size_ = 0;
	    resident_preload_mode_ = use_aligned_mmap ? "full_file_mmap_2mb" : "full_file";
    resident_preload_time_ms_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    resident_parallel_view_bytes_ = resident_parallel_view_bytes;
    resident_parallel_view_build_ms_ = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - parallel_build_start).count();
    resident_stage1_envelope_bytes_ = resident_stage1_envelope_bytes;
    return Status::OK();
}

const ClusterStoreReader::ResidentClusterView*
ClusterStoreReader::GetResidentClusterView(uint32_t cluster_id) const {
    auto it = resident_clusters_.find(cluster_id);
    if (it == resident_clusters_.end()) return nullptr;
    return &it->second;
}

const query::ParsedCluster* ClusterStoreReader::GetResidentParsedCluster(
    uint32_t cluster_id) const {
    auto it = resident_parsed_clusters_.find(cluster_id);
    if (it == resident_parsed_clusters_.end()) return nullptr;
    return &it->second;
}

AddressEntry ClusterStoreReader::GetAddress(uint32_t cluster_id,
                                            uint32_t record_idx) const {
    auto it = loaded_clusters_.find(cluster_id);
    if (it == loaded_clusters_.end()) {
        return AddressEntry{0, 0};
    }
    if (it->second.address_format == AddressFormat::V2RawTable) {
        if (record_idx >= it->second.raw_addresses_v2.size()) {
            return AddressEntry{0, 0};
        }
        return AddressColumn::DecodeRawEntryV2(
            it->second.raw_addresses_v2[record_idx],
            it->second.address_layout.page_size);
    }
    if (record_idx >= it->second.decoded_addresses.size()) {
        return AddressEntry{0, 0};
    }
    return it->second.decoded_addresses[record_idx];
}

std::vector<AddressEntry> ClusterStoreReader::GetAddresses(
    uint32_t cluster_id,
    const std::vector<uint32_t>& indices) const {
    std::vector<AddressEntry> results(indices.size());
    for (size_t i = 0; i < indices.size(); ++i) {
        results[i] = GetAddress(cluster_id, indices[i]);
    }
    return results;
}

Status ClusterStoreReader::LoadCode(uint32_t cluster_id,
                                    uint32_t record_idx,
                                    std::vector<uint64_t>& out_code) const {
    if (fd_ < 0) {
        return Status::InvalidArgument("ClusterStoreReader not open");
    }

    auto it = loaded_clusters_.find(cluster_id);
    if (it == loaded_clusters_.end()) {
        return Status::InvalidArgument(
            "Cluster not loaded — call EnsureClusterLoaded first");
    }

    auto cid_it = cluster_index_.find(cluster_id);
    if (cid_it == cluster_index_.end()) {
        return Status::InvalidArgument("Cluster not found");
    }
    const auto& entry = info_.lookup_table[cid_it->second];
    if (record_idx >= entry.num_records) {
        return Status::InvalidArgument("Record index out of range");
    }

    const uint32_t nwords = num_code_words();
    const uint32_t dim = info_.dim;
    const uint32_t block_idx = record_idx / 32;
    const uint32_t vec_in_block = record_idx % 32;
    const uint32_t fb_bytes = fastscan_block_bytes();
    const uint8_t* block_data =
        it->second.codes_buffer.data() + static_cast<size_t>(block_idx) * fb_bytes;

    out_code.resize(nwords);
    UnpackSignBitsFromFastScan(block_data, vec_in_block, dim, out_code.data());
    return Status::OK();
}

Status ClusterStoreReader::LoadCodes(
    uint32_t cluster_id,
    const std::vector<uint32_t>& indices,
    std::vector<rabitq::RaBitQCode>& out_codes) const {
    if (fd_ < 0) {
        return Status::InvalidArgument("ClusterStoreReader not open");
    }

    out_codes.resize(indices.size());
    const uint32_t dim = info_.dim;
    const uint32_t fb_bytes = fastscan_block_bytes();
    const uint32_t packed_sz = fastscan_packed_size();
    const uint32_t ex_entry_sz = exrabitq_entry_size();

    auto cid_it = cluster_index_.find(cluster_id);
    if (cid_it == cluster_index_.end()) {
        return Status::InvalidArgument("Cluster not found");
    }
    const auto& entry = info_.lookup_table[cid_it->second];

    for (size_t i = 0; i < indices.size(); ++i) {
        std::vector<uint64_t> code;
        VDB_RETURN_IF_ERROR(LoadCode(cluster_id, indices[i], code));
        out_codes[i].code = std::move(code);
        out_codes[i].bits = info_.rabitq_config.active_code_bits();

        const auto& cluster_data = loaded_clusters_.at(cluster_id);
        const uint32_t block_idx = indices[i] / 32;
        const uint32_t vec_in_block = indices[i] % 32;
        const uint8_t* block_data =
            cluster_data.codes_buffer.data() + static_cast<size_t>(block_idx) * fb_bytes;
        const float* norms = reinterpret_cast<const float*>(block_data + packed_sz);
        out_codes[i].norm = norms[vec_in_block];

        out_codes[i].sum_x = 0;
        for (auto w : out_codes[i].code) {
            out_codes[i].sum_x += __builtin_popcountll(w);
        }

        if (info_.rabitq_config.stage2_payload_bits() > 0 &&
            (file_version_ >= kFileVersionV11 || ex_entry_sz > 0)) {
            const uint32_t region1_size = entry.num_fastscan_blocks * fb_bytes;
            const uint32_t sign_bytes = exrabitq_sign_bytes();
            const bool official = info_.rabitq_config.uses_official_1_plus_n();
            out_codes[i].ex_sign_packed.resize(official ? 0 : PackedSignBytes(dim));
            if (file_version_ >= kFileVersionV11) {
                const uint32_t batch_size = exrabitq_batch_size();
                const uint32_t dim_block = exrabitq_dim_block();
                const uint32_t num_dim_blocks = exrabitq_dim_block_count();
                const uint32_t sign_block_bytes = dim_block / 8;
                const uint32_t abs_lane_bytes = exrabitq_abs_bytes_per_lane_dim_block();
                const uint32_t batch_block_size = exrabitq_batch_block_size();
                const uint32_t block_id = indices[i] / batch_size;
                const uint32_t lane_id = indices[i] % batch_size;
                const RaBitQExDataLayout exdata_layout =
                    info_.rabitq_config.effective_exdata_layout();
                const bool variable_ex_blocks =
                    file_version_ >= kFileVersionV15 &&
                    UsesVariableOfficialExDataLayout(exdata_layout);
                const uint8_t* block = nullptr;
                uint32_t block_bytes = batch_block_size;
                if (variable_ex_blocks) {
                    const uint8_t* region = cluster_data.codes_buffer.data() + region1_size;
                    uint32_t magic = 0;
                    uint32_t stored_batch_blocks = 0;
                    std::memcpy(&magic, region, sizeof(uint32_t));
                    std::memcpy(&stored_batch_blocks, region + 4, sizeof(uint32_t));
                    if (magic != kVariableExRegionMagic || block_id >= stored_batch_blocks) {
                        return Status::Corruption("Variable ExRaBitQ region header mismatch");
                    }
                    const uint32_t* offsets =
                        reinterpret_cast<const uint32_t*>(region + 8);
                    const uint32_t begin = offsets[block_id];
                    const uint32_t end = offsets[block_id + 1];
                    if (end < begin || end > cluster_data.codes_buffer.size() - region1_size) {
                        return Status::Corruption("Variable ExRaBitQ block offset out of range");
                    }
                    block = region + begin;
                    block_bytes = end - begin;
                } else {
                    block =
                        cluster_data.codes_buffer.data() + region1_size +
                        static_cast<size_t>(block_id) * batch_block_size;
                }
                const uint8_t* payload = block + sizeof(uint32_t);
                const uint8_t* abs_base = payload;
                uint32_t valid_count = batch_size;
                std::memcpy(&valid_count, block, sizeof(uint32_t));
                if (valid_count > batch_size || lane_id >= valid_count) {
                    return Status::InvalidArgument("Record index points to invalid batch lane");
                }
                const uint32_t stored_lanes = variable_ex_blocks ? valid_count : batch_size;
                const uint8_t* sign_base =
                    abs_base + num_dim_blocks * stored_lanes * abs_lane_bytes;
                out_codes[i].ex_code.resize(dim);
                if (!official) {
                    std::fill(out_codes[i].ex_sign_packed.begin(),
                              out_codes[i].ex_sign_packed.end(), 0);
                }
                if (variable_ex_blocks && IsVectorBitMajorTileLayout(exdata_layout)) {
                    const uint32_t vector_bytes =
                        simd::ExRaBitQBitMajorTileVectorBytes(
                            dim, info_.rabitq_config.stage2_payload_bits());
                    if (vector_bytes == 0) {
                        return Status::Corruption(
                            "Invalid vector_bitmajor_tiles payload width");
                    }
                    const uint8_t* abs_ptr =
                        abs_base + static_cast<size_t>(lane_id) * vector_bytes;
                    if (!simd::ExRaBitQUnpackOfficialBitMajorTiles(
                            abs_ptr, dim, info_.rabitq_config.stage2_payload_bits(),
                            out_codes[i].ex_code.data())) {
                        return Status::Corruption(
                            "Failed to unpack vector_bitmajor_tiles payload");
                    }
                } else {
                const uint8_t* sparse_cursor = abs_base;
                for (uint32_t db = 0; db < num_dim_blocks; ++db) {
                    const uint32_t dim_start = db * dim_block;
                    const uint32_t copy = std::min(dim_block, dim - dim_start);
                    uint8_t sparse_lane_buf[32] = {0};
                    const uint8_t* abs_ptr = nullptr;
                    if (variable_ex_blocks &&
                        exdata_layout == RaBitQExDataLayout::kSplit3ZeroPlaneElide) {
                        for (uint32_t plane = 0; plane < info_.rabitq_config.stage2_payload_bits();
                             ++plane) {
                            const uint8_t mask = *sparse_cursor++;
                            const uint32_t rank =
                                PopcountLow8(mask & ((1u << lane_id) - 1u));
                            const uint32_t count = PopcountLow8(mask);
                            if ((mask & (1u << lane_id)) != 0) {
                                std::memcpy(sparse_lane_buf + plane * sizeof(uint64_t),
                                            sparse_cursor + rank * sizeof(uint64_t),
                                            sizeof(uint64_t));
                            }
                            sparse_cursor += count * sizeof(uint64_t);
                        }
                        abs_ptr = sparse_lane_buf;
	                    } else if (variable_ex_blocks &&
	                               IsVectorContiguousOfficialLayout(exdata_layout)) {
	                        abs_ptr =
	                            abs_base +
	                            (static_cast<size_t>(lane_id) * num_dim_blocks + db) *
	                                abs_lane_bytes;
	                    } else if (variable_ex_blocks &&
	                               IsSmallLaneBitplanesLayout(exdata_layout)) {
	                        const uint32_t subgroup_lanes =
	                            SmallLaneBitplanesGroupSize(exdata_layout);
	                        const uint32_t group_start =
	                            (lane_id / subgroup_lanes) * subgroup_lanes;
	                        const uint32_t group_lanes =
	                            std::min<uint32_t>(subgroup_lanes, valid_count - group_start);
	                        const uint32_t local_lane = lane_id - group_start;
	                        const uint8_t* group_base =
	                            abs_base +
	                            static_cast<size_t>(group_start) * num_dim_blocks *
	                                abs_lane_bytes;
	                        abs_ptr =
	                            group_base +
	                            (static_cast<size_t>(db) * group_lanes + local_lane) *
	                                abs_lane_bytes;
	                    } else {
	                        abs_ptr =
	                            abs_base + static_cast<size_t>(db) * stored_lanes * abs_lane_bytes +
	                            lane_id * abs_lane_bytes;
                    }
                    if (exrabitq_magnitude_packed()) {
                        uint8_t unpacked_abs[64] = {0};
                        const bool direct =
                            official && RaBitQExDataLayoutIsDirect(exdata_layout);
                        bool unpacked_ok = false;
                        if (direct) {
                            unpacked_ok =
                                exdata_layout == RaBitQExDataLayout::kSplit3TwoPlusOne
                                    ? simd::ExRaBitQUnpackOfficialDirect3(
                                          abs_ptr, dim_block, exdata_layout, unpacked_abs)
                                : exdata_layout == RaBitQExDataLayout::kVectorNibble4
                                    ? simd::ExRaBitQUnpackOfficialNibble4(
                                          abs_ptr, dim_block, unpacked_abs)
                                : exdata_layout == RaBitQExDataLayout::kVector2Bit
                                    ? simd::ExRaBitQUnpackOfficial2Bit(
                                          abs_ptr, dim_block, unpacked_abs)
                                    : simd::ExRaBitQUnpackOfficialDirectBitplanes(
                                          abs_ptr, dim_block,
                                          info_.rabitq_config.stage2_payload_bits(),
                                          unpacked_abs);
                        } else {
                            unpacked_ok = simd::ExRaBitQUnpackMagnitudes(
                                abs_ptr, dim_block,
                                info_.rabitq_config.stage2_payload_bits(),
                                unpacked_abs);
                        }
                        if (!unpacked_ok) {
                            return Status::Corruption(
                                "Failed to unpack ExRaBitQ packed magnitude block");
                        }
                        std::memcpy(out_codes[i].ex_code.data() + dim_start,
                                    unpacked_abs, copy);
                    } else {
                        std::memcpy(out_codes[i].ex_code.data() + dim_start,
                                    abs_ptr, copy);
                    }
                    if (!official) {
                        const uint8_t* sign_ptr =
                            sign_base + static_cast<size_t>(db) * batch_size * sign_block_bytes +
                            lane_id * sign_block_bytes;
                        const uint32_t sign_offset = dim_start / 8;
                        const uint32_t sign_copy =
                            sign_offset < out_codes[i].ex_sign_packed.size()
                                ? std::min<uint32_t>(
                                      sign_block_bytes,
                                      static_cast<uint32_t>(
                                          out_codes[i].ex_sign_packed.size() - sign_offset))
                                : 0;
                        if (sign_copy > 0) {
                            std::memcpy(out_codes[i].ex_sign_packed.data() + sign_offset,
                                        sign_ptr, sign_copy);
                        }
                    }
                }
                }
                if (official) {
                    const float* factors = reinterpret_cast<const float*>(
                        block + block_bytes - stored_lanes * 2u * sizeof(float));
                    out_codes[i].ex_code_sign_folded = true;
                    out_codes[i].ex_factor_add = factors[lane_id];
                    out_codes[i].ex_factor_rescale = factors[stored_lanes + lane_id];
                    out_codes[i].xipnorm =
                        out_codes[i].norm > 1e-30f
                            ? -0.5f * out_codes[i].ex_factor_rescale /
                                  out_codes[i].norm
                            : 0.0f;
                } else {
                    const float* xipnorms = reinterpret_cast<const float*>(
                        block + block_bytes - stored_lanes * sizeof(float));
                    out_codes[i].xipnorm = xipnorms[lane_id];
                }
            } else {
                const uint8_t* ex_ptr = cluster_data.codes_buffer.data() + region1_size +
                                        static_cast<size_t>(indices[i]) * ex_entry_sz;
                out_codes[i].ex_code.assign(ex_ptr, ex_ptr + dim);
                if (file_version_ >= kFileVersionV10) {
                    out_codes[i].ex_sign_packed.assign(ex_ptr + dim,
                                                       ex_ptr + dim + sign_bytes);
                } else {
                    std::fill(out_codes[i].ex_sign_packed.begin(),
                              out_codes[i].ex_sign_packed.end(), 0);
                    const uint8_t* legacy_sign = ex_ptr + dim;
                    for (uint32_t d = 0; d < dim; ++d) {
                        if (legacy_sign[d]) {
                            out_codes[i].ex_sign_packed[d / 8] |=
                                static_cast<uint8_t>(1u << (d % 8));
                        }
                    }
                }
                std::memcpy(&out_codes[i].xipnorm, ex_ptr + dim + sign_bytes,
                            sizeof(float));
            }
        }
    }

    return Status::OK();
}

const uint8_t* ClusterStoreReader::GetCodePtr(uint32_t cluster_id,
                                              uint32_t record_idx) const {
    auto it = loaded_clusters_.find(cluster_id);
    if (it == loaded_clusters_.end()) return nullptr;

    const uint32_t fb_bytes = fastscan_block_bytes();
    const uint32_t block_idx = record_idx / 32;
    const uint32_t offset = block_idx * fb_bytes;
    if (offset + fb_bytes > it->second.codes_buffer.size()) return nullptr;
    return it->second.codes_buffer.data() + offset;
}

}  // namespace storage
}  // namespace vdb
