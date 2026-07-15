#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

#include "vdb/query/async_reader.h"

namespace fs = std::filesystem;
using vdb::Status;
using vdb::query::IoCompletion;
using vdb::query::IoUringReader;

namespace {

struct SeparateStoreMapHeader {
    char magic[8];
    uint32_t version;
    uint32_t record_size;
    uint64_t count;
    uint32_t vec_bytes;
    uint32_t reserved;
};

struct SeparateStoreMapRecord {
    uint64_t combined_offset;
    uint64_t row_id;
    uint64_t payload_offset;
    uint32_t payload_bytes;
    uint32_t reserved;
};

static_assert(sizeof(SeparateStoreMapHeader) == 32);
static_assert(sizeof(SeparateStoreMapRecord) == 32);

struct TraceEntry {
    uint32_t query_index = 0;
    uint32_t request_index = 0;
    uint64_t combined_offset = 0;
    uint64_t dense_offset = 0;
    uint32_t read_length = 0;
    uint8_t request_type = 0;
};

struct ProcIo {
    uint64_t rchar = 0;
    uint64_t syscr = 0;
    uint64_t read_bytes = 0;
};

struct ReplayMetrics {
    double wall_ms = 0.0;
    std::vector<double> query_ms;
    std::vector<double> request_us;
    uint64_t logical_bytes = 0;
    uint64_t requests = 0;
};

static std::string GetArg(int argc, char** argv, const char* key,
                          const std::string& def = "") {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    }
    return def;
}

static int GetIntArg(int argc, char** argv, const char* key, int def) {
    const std::string value = GetArg(argc, argv, key, "");
    return value.empty() ? def : std::atoi(value.c_str());
}

static int Usage() {
    std::fprintf(stderr,
        "Usage: bench_layout_io_replay --trace FILE --map FILE "
        "--combined-file FILE --vector-file FILE --layout combined|dense "
        "--cache-mode cold|warm --output DIR [--queue-depth 64] "
        "[--batch-size 32] [--max-queries 0]\n");
    return 2;
}

static double Percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double index = p * static_cast<double>(values.size() - 1);
    const size_t lo = static_cast<size_t>(index);
    const size_t hi = std::min(values.size() - 1, lo + 1);
    const double frac = index - static_cast<double>(lo);
    return values[lo] + (values[hi] - values[lo]) * frac;
}

static ProcIo ReadProcIo() {
    ProcIo out;
    std::ifstream f("/proc/self/io");
    std::string key;
    uint64_t value = 0;
    while (f >> key >> value) {
        if (key == "rchar:") out.rchar = value;
        if (key == "syscr:") out.syscr = value;
        if (key == "read_bytes:") out.read_bytes = value;
    }
    return out;
}

static bool LoadDenseOffsets(const std::string& path,
                             std::unordered_map<uint64_t, uint64_t>* offsets,
                             uint32_t* vec_bytes) {
    std::ifstream f(path, std::ios::binary);
    SeparateStoreMapHeader header{};
    f.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!f.good() || header.version != 1 ||
        header.record_size != sizeof(SeparateStoreMapRecord)) {
        return false;
    }
    offsets->reserve(static_cast<size_t>(header.count * 1.3));
    for (uint64_t i = 0; i < header.count; ++i) {
        SeparateStoreMapRecord record{};
        f.read(reinterpret_cast<char*>(&record), sizeof(record));
        if (!f.good()) return false;
        (*offsets)[record.combined_offset] =
            record.row_id * static_cast<uint64_t>(header.vec_bytes);
    }
    *vec_bytes = header.vec_bytes;
    return true;
}

static bool LoadTrace(const std::string& path,
                      const std::unordered_map<uint64_t, uint64_t>& offsets,
                      uint32_t max_queries,
                      std::vector<TraceEntry>* entries) {
    std::ifstream f(path);
    std::string line;
    if (!std::getline(f, line)) return false;
    while (std::getline(f, line)) {
        TraceEntry entry;
        unsigned long long combined_offset = 0;
        unsigned request_type = 0;
        if (std::sscanf(line.c_str(), "%u,%u,%llu,%u,%u",
                        &entry.query_index, &entry.request_index,
                        &combined_offset,
                        &entry.read_length, &request_type) != 5) {
            return false;
        }
        entry.combined_offset = static_cast<uint64_t>(combined_offset);
        if (max_queries > 0 && entry.query_index >= max_queries) continue;
        const auto it = offsets.find(entry.combined_offset);
        if (it == offsets.end()) {
            std::fprintf(stderr, "Missing map entry for offset=%llu\n",
                         static_cast<unsigned long long>(entry.combined_offset));
            return false;
        }
        entry.dense_offset = it->second;
        entry.request_type = static_cast<uint8_t>(request_type);
        entries->push_back(entry);
    }
    return !entries->empty();
}

static void Complete(IoUringReader* reader,
                     std::vector<IoCompletion>* completions,
                     std::vector<bool>* in_use,
                     const std::vector<uint32_t>* expected_lengths,
                     std::vector<std::chrono::steady_clock::time_point>* starts,
                     std::vector<uint16_t>* free_slots,
                     std::vector<double>* request_us,
                     bool wait) {
    uint32_t count = wait
        ? reader->WaitAndPoll(completions->data(), completions->size())
        : reader->Poll(completions->data(), completions->size());
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t token = (*completions)[i].user_data;
        if (token == 0 || token > in_use->size()) {
            std::fprintf(stderr, "Invalid completion token=%llu\n",
                         static_cast<unsigned long long>(token));
            std::abort();
        }
        const uint16_t slot = static_cast<uint16_t>(token - 1);
        if (!(*in_use)[slot] || (*completions)[i].result < 0 ||
            static_cast<uint32_t>((*completions)[i].result) !=
                (*expected_lengths)[slot]) {
            std::fprintf(stderr,
                         "I/O completion failed slot=%u result=%d expected=%u\n",
                         slot, (*completions)[i].result,
                         (*expected_lengths)[slot]);
            std::abort();
        }
        const double us = std::chrono::duration<double, std::micro>(
            std::chrono::steady_clock::now() - (*starts)[slot]).count();
        request_us->push_back(us);
        (*in_use)[slot] = false;
        free_slots->push_back(slot);
    }
}

static ReplayMetrics Replay(int fd, const std::vector<TraceEntry>& entries,
                            bool dense, uint32_t queue_depth,
                            uint32_t batch_size, bool collect) {
    auto reader = std::make_unique<IoUringReader>();
    Status status = reader->Init(
        queue_depth, std::max<uint32_t>(4096, queue_depth * 4), false, false);
    if (!status.ok()) {
        std::fprintf(stderr, "io_uring init failed: %s\n",
                     status.ToString().c_str());
        std::abort();
    }
    status = reader->RegisterFiles(&fd, 1);
    if (!status.ok()) {
        std::fprintf(stderr, "file registration failed: %s\n",
                     status.ToString().c_str());
        std::abort();
    }

    std::vector<uint8_t*> buffers(queue_depth, nullptr);
    std::vector<const uint8_t*> const_buffers(queue_depth, nullptr);
    std::vector<uint32_t> capacities(queue_depth, 4096);
    for (uint32_t i = 0; i < queue_depth; ++i) {
        if (posix_memalign(reinterpret_cast<void**>(&buffers[i]), 4096, 4096) != 0) {
            std::fprintf(stderr, "buffer allocation failed\n");
            std::abort();
        }
        const_buffers[i] = buffers[i];
    }
    status = reader->RegisterBuffers(
        const_buffers.data(), capacities.data(), queue_depth);
    if (!status.ok()) {
        std::fprintf(stderr, "buffer registration failed: %s\n",
                     status.ToString().c_str());
        std::abort();
    }

    std::vector<uint16_t> free_slots;
    free_slots.reserve(queue_depth);
    for (uint32_t i = 0; i < queue_depth; ++i) {
        free_slots.push_back(static_cast<uint16_t>(queue_depth - 1 - i));
    }
    std::vector<bool> in_use(queue_depth, false);
    std::vector<uint32_t> expected_lengths(queue_depth, 0);
    std::vector<std::chrono::steady_clock::time_point> starts(queue_depth);
    std::vector<IoCompletion> completions(queue_depth);

    ReplayMetrics metrics;
    if (collect) {
        metrics.request_us.reserve(entries.size());
        metrics.query_ms.reserve(entries.back().query_index + 1);
    }
    const auto wall_start = std::chrono::steady_clock::now();
    auto submit_or_abort = [&]() {
        if (reader->prepped() == 0) return;
        const uint32_t submitted = reader->Submit();
        if (submitted == 0) {
            std::fprintf(stderr, "io_uring submit made no progress\n");
            std::abort();
        }
    };
    size_t head = 0;
    while (head < entries.size()) {
        const uint32_t query_index = entries[head].query_index;
        const auto query_start = std::chrono::steady_clock::now();
        while (head < entries.size() && entries[head].query_index == query_index) {
            if (free_slots.empty()) {
                submit_or_abort();
                Complete(reader.get(), &completions, &in_use,
                         &expected_lengths, &starts, &free_slots,
                         &metrics.request_us, true);
            }
            const TraceEntry& entry = entries[head++];
            const uint16_t slot = free_slots.back();
            free_slots.pop_back();
            in_use[slot] = true;
            expected_lengths[slot] = entry.read_length;
            starts[slot] = std::chrono::steady_clock::now();
            const uint64_t offset = dense ? entry.dense_offset
                                          : entry.combined_offset;
            status = reader->PrepReadRegisteredBufferFixedFileTagged(
                0, buffers[slot], slot, entry.read_length, offset,
                static_cast<uint64_t>(slot) + 1);
            if (!status.ok()) {
                std::fprintf(stderr, "prep read failed: %s\n",
                             status.ToString().c_str());
                std::abort();
            }
            metrics.logical_bytes += entry.read_length;
            metrics.requests++;
            if (reader->prepped() >= batch_size) {
                submit_or_abort();
                Complete(reader.get(), &completions, &in_use,
                         &expected_lengths, &starts, &free_slots,
                         &metrics.request_us, false);
            }
        }
        submit_or_abort();
        while (reader->InFlight() > 0) {
            Complete(reader.get(), &completions, &in_use,
                     &expected_lengths, &starts, &free_slots,
                     &metrics.request_us, true);
        }
        if (collect) {
            metrics.query_ms.push_back(std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - query_start).count());
        }
    }
    metrics.wall_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - wall_start).count();
    // Queue teardown unregisters and unpins fixed buffers before they are freed.
    reader.reset();
    for (uint8_t* buffer : buffers) std::free(buffer);
    return metrics;
}

struct PageGeometry {
    uint64_t unique_pages = 0;
    uint64_t page_touches = 0;
    double avg_unique_pages_per_query = 0.0;
    double avg_logical_bytes_per_query = 0.0;
    double estimated_page_amplification = 0.0;
};

static PageGeometry ComputePageGeometry(const std::vector<TraceEntry>& entries,
                                        bool dense) {
    constexpr uint64_t kPage = 4096;
    std::unordered_set<uint64_t> global_pages;
    std::unordered_set<uint64_t> query_pages;
    uint32_t current_query = entries.front().query_index;
    uint64_t query_count = 0;
    uint64_t sum_query_pages = 0;
    uint64_t logical_bytes = 0;
    PageGeometry geometry;
    auto finish_query = [&]() {
        sum_query_pages += query_pages.size();
        query_pages.clear();
        query_count++;
    };
    for (const TraceEntry& entry : entries) {
        if (entry.query_index != current_query) {
            finish_query();
            current_query = entry.query_index;
        }
        const uint64_t offset = dense ? entry.dense_offset
                                      : entry.combined_offset;
        const uint64_t first = offset / kPage;
        const uint64_t last = (offset + entry.read_length - 1) / kPage;
        for (uint64_t page = first; page <= last; ++page) {
            global_pages.insert(page);
            query_pages.insert(page);
            geometry.page_touches++;
        }
        logical_bytes += entry.read_length;
    }
    finish_query();
    geometry.unique_pages = global_pages.size();
    geometry.avg_unique_pages_per_query = query_count > 0
        ? static_cast<double>(sum_query_pages) / query_count : 0.0;
    geometry.avg_logical_bytes_per_query = query_count > 0
        ? static_cast<double>(logical_bytes) / query_count : 0.0;
    geometry.estimated_page_amplification =
        geometry.avg_logical_bytes_per_query > 0.0
            ? geometry.avg_unique_pages_per_query * kPage /
                  geometry.avg_logical_bytes_per_query
            : 0.0;
    return geometry;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string trace_path = GetArg(argc, argv, "--trace");
    const std::string map_path = GetArg(argc, argv, "--map");
    const std::string combined_path = GetArg(argc, argv, "--combined-file");
    const std::string vector_path = GetArg(argc, argv, "--vector-file");
    const std::string layout = GetArg(argc, argv, "--layout");
    const std::string cache_mode = GetArg(argc, argv, "--cache-mode", "cold");
    const std::string output_dir = GetArg(argc, argv, "--output");
    const uint32_t queue_depth = static_cast<uint32_t>(
        std::max(1, GetIntArg(argc, argv, "--queue-depth", 64)));
    const uint32_t batch_size = static_cast<uint32_t>(
        std::max(1, GetIntArg(argc, argv, "--batch-size", 32)));
    const uint32_t max_queries = static_cast<uint32_t>(
        std::max(0, GetIntArg(argc, argv, "--max-queries", 0)));
    if (trace_path.empty() || map_path.empty() || combined_path.empty() ||
        vector_path.empty() || output_dir.empty() ||
        (layout != "combined" && layout != "dense") ||
        (cache_mode != "cold" && cache_mode != "warm")) {
        return Usage();
    }

    std::unordered_map<uint64_t, uint64_t> offsets;
    uint32_t vec_bytes = 0;
    if (!LoadDenseOffsets(map_path, &offsets, &vec_bytes)) {
        std::fprintf(stderr, "Failed to load map: %s\n", map_path.c_str());
        return 1;
    }
    std::vector<TraceEntry> entries;
    if (!LoadTrace(trace_path, offsets, max_queries, &entries)) {
        std::fprintf(stderr, "Failed to load trace: %s\n", trace_path.c_str());
        return 1;
    }
    const bool dense = layout == "dense";
    const std::string target_path = dense ? vector_path : combined_path;
    const int fd = ::open(target_path.c_str(), O_RDONLY);
    if (fd < 0) {
        std::perror(target_path.c_str());
        return 1;
    }

    int fadvise_result = 0;
    if (cache_mode == "cold") {
        fadvise_result = ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    } else {
        (void)Replay(fd, entries, dense, queue_depth, batch_size, false);
    }

    const ProcIo io_before = ReadProcIo();
    struct rusage usage_before {};
    struct rusage usage_after {};
    getrusage(RUSAGE_SELF, &usage_before);
    ReplayMetrics metrics = Replay(fd, entries, dense, queue_depth, batch_size, true);
    getrusage(RUSAGE_SELF, &usage_after);
    const ProcIo io_after = ReadProcIo();
    const PageGeometry geometry = ComputePageGeometry(entries, dense);
    ::close(fd);

    const double avg_query_ms = metrics.query_ms.empty() ? 0.0
        : std::accumulate(metrics.query_ms.begin(), metrics.query_ms.end(), 0.0) /
              metrics.query_ms.size();
    const double avg_request_us = metrics.request_us.empty() ? 0.0
        : std::accumulate(metrics.request_us.begin(), metrics.request_us.end(), 0.0) /
              metrics.request_us.size();
    const double qps = metrics.wall_ms > 0.0
        ? metrics.query_ms.size() * 1000.0 / metrics.wall_ms : 0.0;

    fs::create_directories(output_dir);
    std::ofstream out(fs::path(output_dir) / "results.json");
    out << "{\n";
    out << "  \"layout\": \"" << layout << "\",\n";
    out << "  \"cache_mode\": \"" << cache_mode << "\",\n";
    out << "  \"target_file\": \"" << target_path << "\",\n";
    out << "  \"trace_file\": \"" << trace_path << "\",\n";
    out << "  \"map_file\": \"" << map_path << "\",\n";
    out << "  \"vec_bytes\": " << vec_bytes << ",\n";
    out << "  \"queries\": " << metrics.query_ms.size() << ",\n";
    out << "  \"requests\": " << metrics.requests << ",\n";
    out << "  \"logical_bytes\": " << metrics.logical_bytes << ",\n";
    out << "  \"wall_ms\": " << metrics.wall_ms << ",\n";
    out << "  \"qps\": " << qps << ",\n";
    out << "  \"avg_query_ms\": " << avg_query_ms << ",\n";
    out << "  \"p50_query_ms\": " << Percentile(metrics.query_ms, 0.50) << ",\n";
    out << "  \"p95_query_ms\": " << Percentile(metrics.query_ms, 0.95) << ",\n";
    out << "  \"p99_query_ms\": " << Percentile(metrics.query_ms, 0.99) << ",\n";
    out << "  \"avg_request_us\": " << avg_request_us << ",\n";
    out << "  \"p95_request_us\": " << Percentile(metrics.request_us, 0.95) << ",\n";
    out << "  \"p99_request_us\": " << Percentile(metrics.request_us, 0.99) << ",\n";
    out << "  \"unique_pages\": " << geometry.unique_pages << ",\n";
    out << "  \"page_touches\": " << geometry.page_touches << ",\n";
    out << "  \"avg_unique_pages_per_query\": "
        << geometry.avg_unique_pages_per_query << ",\n";
    out << "  \"avg_logical_bytes_per_query\": "
        << geometry.avg_logical_bytes_per_query << ",\n";
    out << "  \"estimated_page_amplification\": "
        << geometry.estimated_page_amplification << ",\n";
    out << "  \"proc_rchar_delta\": " << (io_after.rchar - io_before.rchar) << ",\n";
    out << "  \"proc_syscr_delta\": " << (io_after.syscr - io_before.syscr) << ",\n";
    out << "  \"proc_read_bytes_delta\": "
        << (io_after.read_bytes - io_before.read_bytes) << ",\n";
    out << "  \"minor_faults_delta\": "
        << (usage_after.ru_minflt - usage_before.ru_minflt) << ",\n";
    out << "  \"major_faults_delta\": "
        << (usage_after.ru_majflt - usage_before.ru_majflt) << ",\n";
    out << "  \"inblock_delta\": "
        << (usage_after.ru_inblock - usage_before.ru_inblock) << ",\n";
    out << "  \"fadvise_result\": " << fadvise_result << "\n";
    out << "}\n";
    out.close();

    std::ofstream per_query(fs::path(output_dir) / "per_query.csv");
    per_query << "query_index,latency_ms\n";
    for (size_t i = 0; i < metrics.query_ms.size(); ++i) {
        per_query << i << ',' << metrics.query_ms[i] << '\n';
    }
    std::printf("layout=%s cache=%s queries=%zu requests=%llu avg_ms=%.6f "
                "qps=%.2f read_bytes=%llu unique_pages=%llu amp=%.3f\n",
                layout.c_str(), cache_mode.c_str(), metrics.query_ms.size(),
                static_cast<unsigned long long>(metrics.requests), avg_query_ms,
                qps,
                static_cast<unsigned long long>(io_after.read_bytes - io_before.read_bytes),
                static_cast<unsigned long long>(geometry.unique_pages),
                geometry.estimated_page_amplification);
    return 0;
}
