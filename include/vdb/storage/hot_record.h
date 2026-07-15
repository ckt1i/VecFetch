#pragma once

#include <cstdint>
#include <cstring>

#include "vdb/common/status.h"

namespace vdb {
namespace storage {

enum class HotPayloadStorageType : uint8_t {
    kInlinePayload = 0,
    kColdPointer = 1,
    kPrefixColdPointer = 2,
};

struct HotPayloadDescriptor {
    uint8_t payload_storage_type = 0;
    uint8_t header_size = sizeof(HotPayloadDescriptor);
    uint16_t flags = 0;
    uint32_t inline_bytes = 0;
    uint64_t payload_offset = 0;
    uint64_t payload_bytes = 0;
};

static_assert(sizeof(HotPayloadDescriptor) == 24,
              "HotPayloadDescriptor must remain a fixed 24-byte header");

inline bool IsKnownHotPayloadStorageType(uint8_t type) {
    return type == static_cast<uint8_t>(HotPayloadStorageType::kInlinePayload) ||
           type == static_cast<uint8_t>(HotPayloadStorageType::kColdPointer) ||
           type == static_cast<uint8_t>(HotPayloadStorageType::kPrefixColdPointer);
}

inline Status ValidateHotPayloadDescriptor(const HotPayloadDescriptor& desc) {
    if (desc.header_size != sizeof(HotPayloadDescriptor)) {
        return Status::InvalidArgument("invalid hot payload descriptor size");
    }
    if (!IsKnownHotPayloadStorageType(desc.payload_storage_type)) {
        return Status::InvalidArgument("unknown hot payload storage type");
    }
    if (desc.payload_storage_type ==
        static_cast<uint8_t>(HotPayloadStorageType::kPrefixColdPointer)) {
        if (desc.inline_bytes == 0 ||
            static_cast<uint64_t>(desc.inline_bytes) >= desc.payload_bytes) {
            return Status::InvalidArgument(
                "invalid prefix-cold hot payload descriptor");
        }
    }
    if (desc.payload_storage_type ==
        static_cast<uint8_t>(HotPayloadStorageType::kInlinePayload)) {
        if (desc.inline_bytes != desc.payload_bytes || desc.payload_offset != 0) {
            return Status::InvalidArgument("invalid inline hot payload descriptor");
        }
    } else if (desc.payload_storage_type ==
               static_cast<uint8_t>(HotPayloadStorageType::kColdPointer)) {
        if (desc.inline_bytes != 0) {
            return Status::InvalidArgument("invalid cold hot payload descriptor");
        }
    }
    return Status::OK();
}

inline HotPayloadDescriptor DecodeHotPayloadDescriptor(const uint8_t* bytes) {
    HotPayloadDescriptor desc{};
    std::memcpy(&desc, bytes, sizeof(desc));
    return desc;
}

inline void EncodeHotPayloadDescriptor(const HotPayloadDescriptor& desc,
                                       uint8_t* bytes) {
    std::memcpy(bytes, &desc, sizeof(desc));
}

}  // namespace storage
}  // namespace vdb
