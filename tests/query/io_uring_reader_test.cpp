#include <gtest/gtest.h>

#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "vdb/query/async_reader.h"

using namespace vdb::query;
namespace fs = std::filesystem;

class IoUringReaderTest : public ::testing::Test {
 protected:
    void SetUp() override {
        test_dir_ = fs::temp_directory_path() / "vdb_iouring_test";
        fs::create_directories(test_dir_);

        // Write test file: 1024 bytes, byte[i] = i % 256
        test_file_ = (test_dir_ / "data.bin").string();
        std::ofstream f(test_file_, std::ios::binary);
        for (uint32_t i = 0; i < 1024; ++i) {
            uint8_t byte = static_cast<uint8_t>(i % 256);
            f.write(reinterpret_cast<const char*>(&byte), 1);
        }

        fd_ = ::open(test_file_.c_str(), O_RDONLY);
        ASSERT_GE(fd_, 0);

        reader_ = std::make_unique<IoUringReader>();
        auto s = reader_->Init(64, 256);
        if (!s.ok()) {
            GTEST_SKIP() << "io_uring not available: " << s.message();
        }
    }

    void TearDown() override {
        reader_.reset();
        if (fd_ >= 0) ::close(fd_);
        fs::remove_all(test_dir_);
    }

    fs::path test_dir_;
    std::string test_file_;
    int fd_ = -1;
    std::unique_ptr<IoUringReader> reader_;
};

TEST_F(IoUringReaderTest, SingleRead) {
    uint8_t buf[16];
    ASSERT_TRUE(reader_->PrepRead(fd_, buf, 16, 0).ok());
    EXPECT_EQ(reader_->Submit(), 1u);

    IoCompletion comp;
    EXPECT_EQ(reader_->WaitAndPoll(&comp, 1), 1u);
    EXPECT_EQ(comp.buffer, buf);
    EXPECT_EQ(comp.result, 16);
    EXPECT_EQ(reader_->InFlight(), 0u);

    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(buf[i], static_cast<uint8_t>(i));
    }
}

TEST_F(IoUringReaderTest, MultipleReads) {
    uint8_t buf1[8], buf2[8], buf3[8];
    ASSERT_TRUE(reader_->PrepRead(fd_, buf1, 8, 0).ok());
    ASSERT_TRUE(reader_->PrepRead(fd_, buf2, 8, 100).ok());
    ASSERT_TRUE(reader_->PrepRead(fd_, buf3, 8, 200).ok());
    EXPECT_EQ(reader_->Submit(), 3u);
    EXPECT_EQ(reader_->InFlight(), 3u);

    IoCompletion comps[3];
    uint32_t total = 0;
    while (total < 3) {
        total += reader_->WaitAndPoll(comps + total, 3 - total);
    }
    EXPECT_EQ(reader_->InFlight(), 0u);

    // Verify all completions arrived (order may vary)
    for (uint32_t i = 0; i < 3; ++i) {
        EXPECT_EQ(comps[i].result, 8);
    }
}

TEST_F(IoUringReaderTest, PollReturnsAllReadyCompletionsWithoutLoss) {
    constexpr uint32_t kReads = 16;
    std::vector<std::array<uint8_t, 8>> buffers(kReads);
    for (uint32_t i = 0; i < kReads; ++i) {
        ASSERT_TRUE(reader_->PrepReadTagged(
                        fd_, buffers[i].data(), buffers[i].size(),
                        (i * 31) % 1000, 1000 + i).ok());
    }
    EXPECT_EQ(reader_->Submit(), kReads);

    usleep(1000);
    IoCompletion first[5];
    const uint32_t first_count = reader_->Poll(first, 5);
    EXPECT_LE(first_count, 5u);
    EXPECT_EQ(reader_->InFlight(), kReads - first_count);

    std::set<uint64_t> tags;
    for (uint32_t i = 0; i < first_count; ++i) {
        EXPECT_EQ(first[i].result, 8);
        tags.insert(first[i].user_data);
    }
    IoCompletion remaining[kReads];
    while (reader_->InFlight() > 0) {
        const uint32_t n = reader_->WaitAndPoll(remaining, kReads);
        ASSERT_GT(n, 0u);
        for (uint32_t i = 0; i < n; ++i) {
            EXPECT_EQ(remaining[i].result, 8);
            tags.insert(remaining[i].user_data);
        }
    }
    EXPECT_EQ(tags.size(), kReads);
}

TEST_F(IoUringReaderTest, WaitAndPollDrainsReadyCompletionsWithoutLoss) {
    constexpr uint32_t kReads = 24;
    std::vector<std::array<uint8_t, 8>> buffers(kReads);
    for (uint32_t i = 0; i < kReads; ++i) {
        ASSERT_TRUE(reader_->PrepReadTagged(
                        fd_, buffers[i].data(), buffers[i].size(),
                        (i * 37) % 1000, 2000 + i).ok());
    }
    EXPECT_EQ(reader_->Submit(), kReads);

    IoCompletion completions[kReads];
    std::set<uint64_t> tags;
    while (reader_->InFlight() > 0) {
        const uint32_t before = reader_->InFlight();
        const uint32_t n = reader_->WaitAndPoll(completions, kReads);
        ASSERT_GT(n, 0u);
        EXPECT_EQ(reader_->InFlight(), before - n);
        for (uint32_t i = 0; i < n; ++i) {
            EXPECT_EQ(completions[i].result, 8);
            tags.insert(completions[i].user_data);
        }
    }
    EXPECT_EQ(tags.size(), kReads);
}

TEST_F(IoUringReaderTest, EmptySubmit) {
    EXPECT_EQ(reader_->Submit(), 0u);
    EXPECT_EQ(reader_->InFlight(), 0u);
}

TEST_F(IoUringReaderTest, PollNonBlocking) {
    IoCompletion comp;
    EXPECT_EQ(reader_->Poll(&comp, 1), 0u);
}

TEST_F(IoUringReaderTest, DetailedPollDiagnosticsTrackGetEvents) {
    reader_->SetDetailedPollTiming(true);
    IoCompletion comp;
    EXPECT_EQ(reader_->Poll(&comp, 1), 0u);
    const AsyncPollDiagnostics diagnostics =
        reader_->last_poll_diagnostics();
    EXPECT_EQ(diagnostics.get_events_calls,
              reader_->defer_taskrun_enabled() ? 1u : 0u);
    if (reader_->defer_taskrun_enabled()) {
        EXPECT_GT(diagnostics.get_events_ns, 0u);
    }
}

TEST_F(IoUringReaderTest, OptionalNoDeferPolicyIsPerReaderAndPreservesReads) {
    if (!reader_->defer_taskrun_enabled()) {
        GTEST_SKIP() << "Kernel fallback cannot validate default defer policy";
    }

    IoUringReader optional_reader;
    IoUringInitOptions options;
    options.use_defer_taskrun = false;
    optional_reader.SetForceAsync(true);
    auto status = optional_reader.Init(64, 256, options);
    if (!status.ok()) {
        GTEST_SKIP() << "io_uring no-defer init unavailable: "
                     << status.message();
    }

    EXPECT_TRUE(reader_->defer_taskrun_enabled());
    EXPECT_FALSE(reader_->force_async_enabled());
    EXPECT_FALSE(optional_reader.defer_taskrun_enabled());
    EXPECT_TRUE(optional_reader.force_async_enabled());

    uint8_t buf[16]{};
    ASSERT_TRUE(optional_reader.PrepRead(fd_, buf, 16, 0).ok());
    EXPECT_EQ(optional_reader.Submit(), 1u);
    IoCompletion completion;
    EXPECT_EQ(optional_reader.WaitAndPoll(&completion, 1), 1u);
    EXPECT_EQ(completion.result, 16);
    for (int i = 0; i < 16; ++i) {
        EXPECT_EQ(buf[i], static_cast<uint8_t>(i));
    }

    optional_reader.SetDetailedPollTiming(true);
    EXPECT_EQ(optional_reader.Poll(&completion, 1), 0u);
    EXPECT_EQ(optional_reader.last_poll_diagnostics().get_events_calls, 0u);
}

TEST_F(IoUringReaderTest, SqpollRequestFallsBackCleanly) {
    IoUringReader sqpoll_reader;
    auto s = sqpoll_reader.Init(64, 256, /*iopoll=*/false, /*sqpoll=*/true);
    if (!s.ok()) {
        GTEST_SKIP() << "io_uring not available for SQPOLL test: " << s.message();
    }

    uint8_t buf[16];
    ASSERT_TRUE(sqpoll_reader.PrepRead(fd_, buf, 16, 0).ok());
    EXPECT_EQ(sqpoll_reader.Submit(), 1u);

    IoCompletion comp;
    EXPECT_EQ(sqpoll_reader.WaitAndPoll(&comp, 1), 1u);
    EXPECT_EQ(comp.result, 16);
    EXPECT_TRUE(sqpoll_reader.queue_depth() >= 64u);
}

TEST_F(IoUringReaderTest, TaggedReadPreservesUserData) {
    uint8_t buf[16];
    constexpr uint64_t kTag = 0xBEEFULL;
    ASSERT_TRUE(reader_->PrepReadTagged(fd_, buf, 16, 0, kTag).ok());
    EXPECT_EQ(reader_->Submit(), 1u);

    IoCompletion comp;
    EXPECT_EQ(reader_->WaitAndPoll(&comp, 1), 1u);
    EXPECT_EQ(comp.user_data, kTag);
    EXPECT_EQ(comp.result, 16);
}

TEST_F(IoUringReaderTest, RegisteredFixedBufferReadWorks) {
    void* raw = std::aligned_alloc(4096, 4096);
    ASSERT_NE(raw, nullptr);
    uint8_t* buf = static_cast<uint8_t*>(raw);
    const uint8_t* bufs[] = {buf};
    uint32_t capacities[] = {4096};

    auto reg_status = reader_->RegisterBuffers(bufs, capacities, 1);
    if (!reg_status.ok()) {
        std::free(raw);
        GTEST_SKIP() << "io_uring buffer registration unavailable: "
                     << reg_status.message();
    }

    constexpr uint64_t kTag = 0xCAFEULL;
    ASSERT_TRUE(reader_->PrepReadRegisteredBufferTagged(
                    fd_, buf, 0, 16, 64, kTag).ok());
    EXPECT_EQ(reader_->Submit(), 1u);

    IoCompletion comp;
    EXPECT_EQ(reader_->WaitAndPoll(&comp, 1), 1u);
    EXPECT_EQ(comp.user_data, kTag);
    EXPECT_EQ(comp.result, 16);
    EXPECT_EQ(buf[0], static_cast<uint8_t>(64));

    std::free(raw);
}

TEST_F(IoUringReaderTest, RegisteredFileIndexAndFixedFilePrepWork) {
    auto reg_file_status = reader_->RegisterFiles(&fd_, 1);
    if (!reg_file_status.ok()) {
        GTEST_SKIP() << "io_uring file registration unavailable: "
                     << reg_file_status.message();
    }

    void* raw = std::aligned_alloc(4096, 4096);
    ASSERT_NE(raw, nullptr);
    uint8_t* buf = static_cast<uint8_t*>(raw);
    const uint8_t* bufs[] = {buf};
    uint32_t capacities[] = {4096};

    auto reg_buf_status = reader_->RegisterBuffers(bufs, capacities, 1);
    if (!reg_buf_status.ok()) {
        std::free(raw);
        GTEST_SKIP() << "io_uring buffer registration unavailable: "
                     << reg_buf_status.message();
    }

    const int fd_index = reader_->RegisteredFileIndex(fd_);
    ASSERT_EQ(fd_index, 0);

    constexpr uint64_t kTag = 0xD00DULL;
    ASSERT_TRUE(reader_->PrepReadRegisteredBufferFixedFileTagged(
                    fd_index, buf, 0, 16, 128, kTag).ok());
    EXPECT_EQ(reader_->Submit(), 1u);

    IoCompletion comp;
    EXPECT_EQ(reader_->WaitAndPoll(&comp, 1), 1u);
    EXPECT_EQ(comp.user_data, kTag);
    EXPECT_EQ(comp.result, 16);
    EXPECT_EQ(buf[0], static_cast<uint8_t>(128));

    std::free(raw);
}
