#include "vdb/query/async_reader.h"

#include <liburing.h>
#include <cerrno>
#include <cstring>
#include <algorithm>
#include <vector>

namespace vdb {
namespace query {

namespace {

struct io_uring_sqe* AcquireSqeWithAutoFlush(struct io_uring* ring,
                                            uint32_t* in_flight,
                                            uint32_t* prepped,
                                            std::string* error) {
    struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
    if (sqe != nullptr) {
        return sqe;
    }
    int ret = io_uring_submit(ring);
    if (ret < 0) {
        if (error != nullptr) {
            *error = std::string("io_uring auto-flush submit failed: ") +
                     strerror(-ret);
        }
        return nullptr;
    }
    *in_flight += *prepped;
    *prepped = 0;
    sqe = io_uring_get_sqe(ring);
    if (sqe == nullptr && error != nullptr) {
        *error = "io_uring SQ full after auto-flush";
    }
    return sqe;
}

}  // namespace

struct IoUringReader::Impl {
    struct io_uring ring;
    bool initialized = false;
    bool defer_taskrun = false;
    bool files_registered = false;
    bool buffers_registered = false;
    // fd → registered index mapping (small, typically 2 entries)
    static constexpr int kMaxRegisteredFds = 8;
    int registered_fds[kMaxRegisteredFds] = {};
    uint32_t num_registered = 0;

    int FindFdIndex(int fd) const {
        for (uint32_t i = 0; i < num_registered; ++i) {
            if (registered_fds[i] == fd) return static_cast<int>(i);
        }
        return -1;
    }
};

IoUringReader::IoUringReader() : impl_(new Impl()) {}

IoUringReader::~IoUringReader() {
    if (impl_->initialized) {
        io_uring_queue_exit(&impl_->ring);
    }
    delete impl_;
}

Status IoUringReader::Init(uint32_t queue_depth, uint32_t cq_entries,
                            bool use_iopoll, bool use_sqpoll) {
    struct io_uring_params params;
    std::memset(&params, 0, sizeof(params));
    params.flags = IORING_SETUP_CQSIZE
                 | IORING_SETUP_SINGLE_ISSUER
                 | IORING_SETUP_DEFER_TASKRUN;
    if (use_sqpoll) params.flags |= IORING_SETUP_SQPOLL;
    if (use_iopoll) params.flags |= IORING_SETUP_IOPOLL;
    params.cq_entries = cq_entries;

    int ret = io_uring_queue_init_params(queue_depth, &impl_->ring, &params);
    if (ret < 0) {
        // First fallback: drop SQPOLL while keeping the existing single-issuer
        // fast path. This keeps the old warm-path behavior when SQPOLL is not
        // supported or blocked by environment constraints.
        if (use_sqpoll) {
            std::memset(&params, 0, sizeof(params));
            params.flags = IORING_SETUP_CQSIZE
                         | IORING_SETUP_SINGLE_ISSUER
                         | IORING_SETUP_DEFER_TASKRUN;
            if (use_iopoll) params.flags |= IORING_SETUP_IOPOLL;
            params.cq_entries = cq_entries;
            ret = io_uring_queue_init_params(queue_depth, &impl_->ring, &params);
        }
    }
    if (ret < 0) {
        // Final fallback: kernel may not support DEFER_TASKRUN / SINGLE_ISSUER.
        std::memset(&params, 0, sizeof(params));
        params.flags = IORING_SETUP_CQSIZE;
        if (use_iopoll) params.flags |= IORING_SETUP_IOPOLL;
        params.cq_entries = cq_entries;
        ret = io_uring_queue_init_params(queue_depth, &impl_->ring, &params);
        if (ret < 0) {
            return Status::NotSupported(
                std::string("io_uring init failed: ") + strerror(-ret));
        }
        impl_->defer_taskrun = false;
    } else {
        impl_->defer_taskrun = true;
    }
    impl_->initialized = true;
    queue_depth_ = queue_depth;
    cq_entries_ = cq_entries;
    defer_taskrun_enabled_ = impl_->defer_taskrun;
    iopoll_enabled_ = (params.flags & IORING_SETUP_IOPOLL) != 0;
    sqpoll_enabled_ = (params.flags & IORING_SETUP_SQPOLL) != 0;
    return Status::OK();
}

Status IoUringReader::RegisterFiles(const int* fds, uint32_t count) {
    if (count > Impl::kMaxRegisteredFds) {
        return Status::InvalidArgument("Too many fds to register");
    }
    int ret = io_uring_register_files(&impl_->ring, fds, count);
    if (ret < 0) {
        return Status::IOError(
            std::string("io_uring_register_files failed: ") + strerror(-ret));
    }
    for (uint32_t i = 0; i < count; ++i) {
        impl_->registered_fds[i] = fds[i];
    }
    impl_->num_registered = count;
    impl_->files_registered = true;
    registered_files_enabled_ = true;
    return Status::OK();
}

Status IoUringReader::RegisterBuffers(const uint8_t* const* bufs,
                                      const uint32_t* capacities,
                                      uint32_t count) {
    std::vector<struct iovec> iovecs(count);
    for (uint32_t i = 0; i < count; ++i) {
        iovecs[i].iov_base = const_cast<uint8_t*>(bufs[i]);
        iovecs[i].iov_len = capacities[i];
    }
    int ret = io_uring_register_buffers(&impl_->ring, iovecs.data(), count);
    if (ret < 0) {
        return Status::IOError(
            std::string("io_uring_register_buffers failed: ") + strerror(-ret));
    }
    impl_->buffers_registered = true;
    registered_buffers_enabled_ = true;
    return Status::OK();
}

Status IoUringReader::PrepReadFixedTagged(int fd_index, uint8_t* buf,
                                          uint32_t len, uint64_t offset,
                                          uint64_t user_data) {
    std::string error;
    struct io_uring_sqe* sqe = AcquireSqeWithAutoFlush(
        &impl_->ring, &in_flight_, &prepped_, &error);
    if (sqe == nullptr) {
        return Status::IOError(error);
    }
    io_uring_prep_read(sqe, fd_index, buf, len, offset);
    sqe->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data64(sqe, user_data);
    ++prepped_;
    return Status::OK();
}

Status IoUringReader::PrepReadFixed(int fd_index, uint8_t* buf,
                                    uint32_t len, uint64_t offset) {
    return PrepReadFixedTagged(fd_index, buf, len, offset,
                               reinterpret_cast<uint64_t>(buf));
}

Status IoUringReader::PrepReadRegisteredBufferTagged(int fd, uint8_t* buf,
                                                     uint16_t buf_index,
                                                     uint32_t len,
                                                     uint64_t offset,
                                                     uint64_t user_data) {
    if (!impl_->buffers_registered) {
        return Status::InvalidArgument("registered buffer requested before registration");
    }
    const int fd_idx = RegisteredFileIndex(fd);
    if (fd_idx >= 0) {
        return PrepReadRegisteredBufferFixedFileTagged(
            fd_idx, buf, buf_index, len, offset, user_data);
    }

    std::string error;
    struct io_uring_sqe* sqe = AcquireSqeWithAutoFlush(
        &impl_->ring, &in_flight_, &prepped_, &error);
    if (sqe == nullptr) {
        return Status::IOError(error);
    }
    io_uring_prep_read_fixed(sqe, fd, buf, len, offset, buf_index);
    io_uring_sqe_set_data64(sqe, user_data);
    ++prepped_;
    return Status::OK();
}

Status IoUringReader::PrepReadRegisteredBufferFixedFileTagged(
    int fd_index, uint8_t* buf, uint16_t buf_index, uint32_t len,
    uint64_t offset, uint64_t user_data) {
    if (!impl_->buffers_registered) {
        return Status::InvalidArgument("registered buffer requested before registration");
    }
    if (fd_index < 0) {
        return Status::InvalidArgument("invalid registered file index");
    }
    std::string error;
    struct io_uring_sqe* sqe = AcquireSqeWithAutoFlush(
        &impl_->ring, &in_flight_, &prepped_, &error);
    if (sqe == nullptr) {
        return Status::IOError(error);
    }
    io_uring_prep_read_fixed(sqe, fd_index, buf, len, offset, buf_index);
    sqe->flags |= IOSQE_FIXED_FILE;
    io_uring_sqe_set_data64(sqe, user_data);
    ++prepped_;
    return Status::OK();
}

int IoUringReader::RegisteredFileIndex(int fd) const {
    return impl_->files_registered ? impl_->FindFdIndex(fd) : -1;
}

Status IoUringReader::PrepReadTagged(int fd, uint8_t* buf,
                                     uint32_t len, uint64_t offset,
                                     uint64_t user_data) {
    std::string error;
    struct io_uring_sqe* sqe = AcquireSqeWithAutoFlush(
        &impl_->ring, &in_flight_, &prepped_, &error);
    if (sqe == nullptr) {
        return Status::IOError(error);
    }
    // Automatically use FIXED_FILE if this fd was registered
    int fd_idx = RegisteredFileIndex(fd);
    if (fd_idx >= 0) {
        io_uring_prep_read(sqe, fd_idx, buf, len, offset);
        sqe->flags |= IOSQE_FIXED_FILE;
    } else {
        io_uring_prep_read(sqe, fd, buf, len, offset);
    }
    io_uring_sqe_set_data64(sqe, user_data);
    ++prepped_;
    return Status::OK();
}

uint32_t IoUringReader::Submit() {
    if (prepped_ == 0) return 0;

    int ret = io_uring_submit(&impl_->ring);
    uint32_t submitted = (ret > 0) ? static_cast<uint32_t>(ret) : 0;
    in_flight_ += submitted;
    prepped_ = 0;
    return submitted;
}

uint32_t IoUringReader::Poll(IoCompletion* out, uint32_t max_count) {
    // Under DEFER_TASKRUN, CQEs won't appear until we explicitly ask the
    // kernel to run deferred task_work.  io_uring_get_events() triggers this
    // without blocking.
    if (impl_->defer_taskrun) {
        io_uring_get_events(&impl_->ring);
    }

    uint32_t count = 0;
    struct io_uring_cqe* cqe;

    while (count < max_count) {
        int ret = io_uring_peek_cqe(&impl_->ring, &cqe);
        if (ret != 0) break;  // No more completions

        out[count].user_data = io_uring_cqe_get_data64(cqe);
        out[count].buffer = reinterpret_cast<uint8_t*>(out[count].user_data);
        out[count].result = cqe->res;
        io_uring_cqe_seen(&impl_->ring, cqe);
        ++count;
        --in_flight_;
    }
    return count;
}

uint32_t IoUringReader::WaitAndPoll(IoCompletion* out, uint32_t max_count) {
    if (max_count == 0 || in_flight_ == 0) return 0;

    // Wait for at least one completion
    struct io_uring_cqe* cqe;
    int ret = io_uring_wait_cqe(&impl_->ring, &cqe);
    if (ret < 0) return 0;

    // Consume this one
    out[0].user_data = io_uring_cqe_get_data64(cqe);
    out[0].buffer = reinterpret_cast<uint8_t*>(out[0].user_data);
    out[0].result = cqe->res;
    io_uring_cqe_seen(&impl_->ring, cqe);
    --in_flight_;

    // Drain any additional ready completions
    uint32_t count = 1;
    while (count < max_count) {
        ret = io_uring_peek_cqe(&impl_->ring, &cqe);
        if (ret != 0) break;

        out[count].user_data = io_uring_cqe_get_data64(cqe);
        out[count].buffer = reinterpret_cast<uint8_t*>(out[count].user_data);
        out[count].result = cqe->res;
        io_uring_cqe_seen(&impl_->ring, cqe);
        ++count;
        --in_flight_;
    }
    return count;
}

uint32_t IoUringReader::InFlight() const {
    return in_flight_;
}

}  // namespace query
}  // namespace vdb
