#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "vdb/query/span_planner.h"

namespace {

using vdb::query::PlanSpanRun;
using vdb::query::SpanPlannerConfig;
using vdb::query::SpanPlannerGroup;
using vdb::query::SpanPlannerItem;
using vdb::query::SpanPlannerMode;
using vdb::query::SpanPlannerScratch;
using vdb::query::SpanPlannerStats;

struct RunSpec {
    size_t item_count;
    uint32_t vector_bytes;
    uint32_t seed;
};

SpanPlannerMode ParseMode(const char* value, bool* ok) {
    *ok = true;
    if (std::strcmp(value, "GV") == 0 ||
        std::strcmp(value, "gv") == 0) {
        return SpanPlannerMode::GV;
    }
    if (std::strcmp(value, "SV") == 0 ||
        std::strcmp(value, "sv") == 0) {
        return SpanPlannerMode::SV;
    }
    if (std::strcmp(value, "GE") == 0 ||
        std::strcmp(value, "ge") == 0) {
        return SpanPlannerMode::GE;
    }
    if (std::strcmp(value, "SE") == 0 ||
        std::strcmp(value, "se") == 0) {
        return SpanPlannerMode::SE;
    }
    *ok = false;
    return SpanPlannerMode::GV;
}

const char* ModeName(SpanPlannerMode mode) {
    switch (mode) {
        case SpanPlannerMode::GV: return "GV";
        case SpanPlannerMode::SV: return "SV";
        case SpanPlannerMode::GE: return "GE";
        case SpanPlannerMode::SE: return "SE";
    }
    return "invalid";
}

std::vector<SpanPlannerItem> BuildRepresentativeRun(const RunSpec& spec) {
    std::vector<SpanPlannerItem> items;
    items.reserve(spec.item_count);
    uint64_t offset = 0;
    for (size_t i = 0; i < spec.item_count; ++i) {
        uint32_t credit = 0;
        if ((i + spec.seed) % 4 == 1) {
            credit = 1536;
        } else if ((i + spec.seed) % 7 == 2) {
            credit = 768;
        }
        items.push_back(SpanPlannerItem{
            offset, spec.vector_bytes, credit});
        const uint64_t gap =
            512u + ((i * 5u + spec.seed * 3u) % 7u) * 256u;
        offset += spec.vector_bytes + gap;
    }
    return items;
}

inline void KeepPlannerResultLive(
    const std::vector<SpanPlannerGroup>& groups,
    const SpanPlannerStats& stats) {
#if defined(__GNUC__) || defined(__clang__)
    asm volatile("" : : "g"(groups.data()), "g"(groups.size()),
                 "g"(stats.physical_bytes) : "memory");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

uint64_t ResultChecksum(SpanPlannerMode mode, size_t item_count,
                        uint64_t iterations,
                        const std::vector<SpanPlannerGroup>& groups,
                        const SpanPlannerStats& stats) {
    uint64_t checksum =
        (static_cast<uint64_t>(mode) + 1u) * 0x9e3779b97f4a7c15ULL;
    checksum ^= item_count * 0xbf58476d1ce4e5b9ULL;
    checksum ^= iterations * 0x94d049bb133111ebULL;
    checksum ^= stats.physical_bytes;
    checksum ^= stats.vector_bytes << 1;
    checksum ^= stats.credited_safein_bytes << 2;
    for (const SpanPlannerGroup& group : groups) {
        checksum ^= (group.begin + 1u) * 0x9e3779b185ebca87ULL;
        checksum ^= (group.end + 1u) * 0xc2b2ae3d27d4eb4fULL;
        checksum ^= group.physical_bytes + (checksum << 7) +
                    (checksum >> 3);
    }
    return checksum;
}

bool RunBenchmark(SpanPlannerMode mode, const RunSpec& spec,
                  uint64_t iterations) {
    const std::vector<SpanPlannerItem> items =
        BuildRepresentativeRun(spec);
    SpanPlannerConfig config;
    config.mode = mode;
    config.alpha_num = 3;
    config.alpha_den = 2;
    config.rho_num = 1;
    config.rho_den = 1;

    SpanPlannerScratch scratch;
    scratch.Reserve(items.size());
    std::vector<SpanPlannerGroup> groups;
    groups.reserve(items.size());
    SpanPlannerStats stats;
    std::string error;

    const uint64_t warmup =
        std::min<uint64_t>(2000, iterations / 10 + 1);
    for (uint64_t i = 0; i < warmup; ++i) {
        if (!PlanSpanRun(config, items.data(), items.size(), &scratch,
                         &groups, &stats, &error)) {
            std::fprintf(stderr, "planner warmup failed: %s\n",
                         error.c_str());
            return false;
        }
        KeepPlannerResultLive(groups, stats);
    }

    const auto start = std::chrono::steady_clock::now();
    for (uint64_t i = 0; i < iterations; ++i) {
        if (!PlanSpanRun(config, items.data(), items.size(), &scratch,
                         &groups, &stats, &error)) {
            std::fprintf(stderr, "planner iteration failed: %s\n",
                         error.c_str());
            return false;
        }
        KeepPlannerResultLive(groups, stats);
    }
    const auto elapsed = std::chrono::steady_clock::now() - start;
    const double ns_per_run =
        std::chrono::duration<double, std::nano>(elapsed).count() /
        static_cast<double>(iterations);
    const uint64_t checksum =
        ResultChecksum(mode, spec.item_count, iterations, groups, stats);

    std::printf("%s,%zu,%llu,%.3f,%zu,%llu,%llu,%llu,%llu\n",
                ModeName(mode), spec.item_count,
                static_cast<unsigned long long>(iterations), ns_per_run,
                groups.size(),
                static_cast<unsigned long long>(stats.physical_bytes),
                static_cast<unsigned long long>(stats.vector_bytes),
                static_cast<unsigned long long>(
                    stats.credited_safein_bytes),
                static_cast<unsigned long long>(checksum));
    return true;
}

void PrintUsage(const char* argv0) {
    std::fprintf(
        stderr,
        "Usage: %s [--mode GV|SV|GE|SE|all] "
        "[--iterations N] [--sweep-m]\n",
        argv0);
}

}  // namespace

int main(int argc, char* argv[]) {
    const char* mode_arg = "all";
    uint64_t iterations = 100000;
    bool sweep_m = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode_arg = argv[++i];
        } else if ((std::strcmp(argv[i], "--iterations") == 0 ||
                    std::strcmp(argv[i], "--iters") == 0) &&
                   i + 1 < argc) {
            char* end = nullptr;
            const unsigned long long parsed =
                std::strtoull(argv[++i], &end, 10);
            if (end == argv[i] || *end != '\0' || parsed == 0) {
                PrintUsage(argv[0]);
                return 2;
            }
            iterations = static_cast<uint64_t>(parsed);
        } else if (std::strcmp(argv[i], "--sweep-m") == 0) {
            sweep_m = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            PrintUsage(argv[0]);
            return 0;
        } else {
            PrintUsage(argv[0]);
            return 2;
        }
    }

    std::vector<SpanPlannerMode> modes;
    if (std::strcmp(mode_arg, "all") == 0) {
        modes = {SpanPlannerMode::GV, SpanPlannerMode::SV,
                 SpanPlannerMode::GE, SpanPlannerMode::SE};
    } else {
        bool ok = false;
        const SpanPlannerMode mode = ParseMode(mode_arg, &ok);
        if (!ok) {
            PrintUsage(argv[0]);
            return 2;
        }
        modes.push_back(mode);
    }

    std::vector<RunSpec> specs;
    if (sweep_m) {
        for (size_t m = 1; m <= 32; ++m) {
            specs.push_back(
                {m, 1536, static_cast<uint32_t>(m % 7 + 1)});
        }
    } else {
        specs = {{16, 1536, 3}, {21, 1024, 5}};
    }
    std::printf(
        "mode,m,iterations,ns_per_run,groups,physical_bytes,"
        "vector_bytes,credited_safein_bytes,checksum\n");
    for (SpanPlannerMode mode : modes) {
        for (const RunSpec& spec : specs) {
            if (!RunBenchmark(mode, spec, iterations)) return 1;
        }
    }
    return 0;
}
