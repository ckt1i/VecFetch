#pragma once

#include <cstdint>
#include <deque>
#include <vector>

#include "vdb/common/status.h"

namespace vdb {
namespace query {

/// I/O completion result
struct IoCompletion {
    uint8_t* buffer;    // From CQE user_data
    uint64_t user_data = 0;
    int32_t result;     // Success: bytes read, Failure: negative errno
};

struct AsyncPollDiagnostics {
    uint32_t get_events_calls = 0;
    uint64_t get_events_ns = 0;
};

// ============================================================================
// AsyncReader — abstract I/O backend interface
// ============================================================================

/// Maps to io_uring primitives: PrepRead → io_uring_prep_read,
/// Submit → io_uring_submit, Poll/WaitAndPoll → CQE consumption.
class AsyncReader {
 public:
    virtual ~AsyncReader() = default;

    /// Prepare a read (fills SQE, does not submit).
    virtual Status PrepReadTagged(int fd, uint8_t* buf,
                                  uint32_t len, uint64_t offset,
                                  uint64_t user_data) = 0;
    Status PrepRead(int fd, uint8_t* buf,
                    uint32_t len, uint64_t offset) {
        return PrepReadTagged(fd, buf, len, offset,
                              reinterpret_cast<uint64_t>(buf));
    }

    /// Submit all prepared SQEs.
    /// @return Number of SQEs submitted
    virtual uint32_t Submit() = 0;

    /// Non-blocking poll for completed requests.
    virtual uint32_t Poll(IoCompletion* out, uint32_t max_count) = 0;

    /// Blocking wait for at least one completion, then drain all ready.
    virtual uint32_t WaitAndPoll(IoCompletion* out, uint32_t max_count) = 0;

    /// Number of in-flight requests.
    virtual uint32_t InFlight() const = 0;

    /// Number of PrepRead()ed SQEs not yet Submit()ed.
    virtual uint32_t prepped() const = 0;

    /// Optional low-level diagnostics for the most recent Poll call.
    /// Readers that do not expose backend details return zeros.
    virtual AsyncPollDiagnostics last_poll_diagnostics() const {
        return {};
    }

    /// Detailed backend timing is disabled by default because it adds clocks
    /// inside the I/O hot path. Scheduler-level counters remain available.
    virtual void SetDetailedPollTiming(bool) {}
};

// ============================================================================
// PreadFallbackReader — synchronous pread fallback
// ============================================================================

/// Used when io_uring is not available. PrepRead records the request,
/// Submit executes all pending preads synchronously.
class PreadFallbackReader : public AsyncReader {
 public:
    PreadFallbackReader() = default;

    Status PrepReadTagged(int fd, uint8_t* buf,
                          uint32_t len, uint64_t offset,
                          uint64_t user_data) override;
    uint32_t Submit() override;
    uint32_t Poll(IoCompletion* out, uint32_t max_count) override;
    uint32_t WaitAndPoll(IoCompletion* out, uint32_t max_count) override;
    uint32_t InFlight() const override;
    uint32_t prepped() const override {
        return static_cast<uint32_t>(pending_.size());
    }

 private:
    struct PendingEntry {
        int fd;
        uint8_t* buf;
        uint32_t len;
        uint64_t offset;
        uint64_t user_data;
    };

    std::vector<PendingEntry> pending_;
    std::deque<IoCompletion> completed_;
};

// ============================================================================
// IoUringReader — io_uring async I/O (declared here, implemented separately)
// ============================================================================

struct IoUringInitOptions {
    bool use_iopoll = false;
    bool use_sqpoll = false;
    bool use_defer_taskrun = true;
};

class IoUringReader : public AsyncReader {
 public:
    IoUringReader();
    ~IoUringReader() override;

    /// Initialize io_uring ring.
    /// @param queue_depth SQ depth
    /// @param cq_entries CQ capacity (via IORING_SETUP_CQSIZE)
    /// @return Status::OK or Status::NotSupported
    Status Init(uint32_t queue_depth = 64, uint32_t cq_entries = 4096,
                bool use_iopoll = false, bool use_sqpoll = false);
    Status Init(uint32_t queue_depth, uint32_t cq_entries,
                const IoUringInitOptions& options);

    /// Register file descriptors for IOSQE_FIXED_FILE optimization.
    /// After registration, use PrepReadFixed with the fd index (0-based).
    Status RegisterFiles(const int* fds, uint32_t count);
    Status RegisterBuffers(const uint8_t* const* bufs,
                           const uint32_t* capacities,
                           uint32_t count);

    Status PrepReadTagged(int fd, uint8_t* buf,
                          uint32_t len, uint64_t offset,
                          uint64_t user_data) override;

    /// PrepRead using a registered fd index (IOSQE_FIXED_FILE).
    Status PrepReadFixed(int fd_index, uint8_t* buf,
                         uint32_t len, uint64_t offset);
    Status PrepReadFixedTagged(int fd_index, uint8_t* buf,
                               uint32_t len, uint64_t offset,
                               uint64_t user_data);
    Status PrepReadRegisteredBufferTagged(int fd, uint8_t* buf,
                                          uint16_t buf_index,
                                          uint32_t len, uint64_t offset,
                                          uint64_t user_data);
    Status PrepReadRegisteredBufferFixedFileTagged(int fd_index, uint8_t* buf,
                                                   uint16_t buf_index,
                                                   uint32_t len,
                                                   uint64_t offset,
                                                   uint64_t user_data);
    int RegisteredFileIndex(int fd) const;

    uint32_t Submit() override;
    uint32_t Poll(IoCompletion* out, uint32_t max_count) override;
    uint32_t WaitAndPoll(IoCompletion* out, uint32_t max_count) override;
    uint32_t InFlight() const override;
    uint32_t prepped() const override { return prepped_; }
    AsyncPollDiagnostics last_poll_diagnostics() const override {
        return last_poll_diagnostics_;
    }
    void SetDetailedPollTiming(bool enabled) override {
        detailed_poll_timing_ = enabled;
    }
    void SetForceAsync(bool enabled) { force_async_ = enabled; }
    uint32_t queue_depth() const { return queue_depth_; }
    uint32_t cq_entries() const { return cq_entries_; }
    bool defer_taskrun_enabled() const { return defer_taskrun_enabled_; }
    bool iopoll_enabled() const { return iopoll_enabled_; }
    bool sqpoll_enabled() const { return sqpoll_enabled_; }
    bool registered_files_enabled() const { return registered_files_enabled_; }
    bool registered_buffers_enabled() const { return registered_buffers_enabled_; }
    bool force_async_enabled() const { return force_async_; }

 private:
    struct Impl;
    Impl* impl_ = nullptr;
    uint32_t in_flight_ = 0;
    uint32_t prepped_ = 0;
    uint32_t queue_depth_ = 0;
    uint32_t cq_entries_ = 0;
    bool defer_taskrun_enabled_ = false;
    bool iopoll_enabled_ = false;
    bool sqpoll_enabled_ = false;
    bool registered_files_enabled_ = false;
    bool registered_buffers_enabled_ = false;
    bool detailed_poll_timing_ = false;
    bool force_async_ = false;
    AsyncPollDiagnostics last_poll_diagnostics_;
};

}  // namespace query
}  // namespace vdb
