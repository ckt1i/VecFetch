#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "vdb/query/span_planner.h"

namespace vdb {
namespace query {
namespace {

using U128 = unsigned __int128;

struct OracleState {
    uint64_t requests = 0;
    uint64_t bytes = 0;
    size_t predecessor = 0;
    bool valid = false;
};

bool SafeInAware(SpanPlannerMode mode) {
    return mode == SpanPlannerMode::SV || mode == SpanPlannerMode::SE;
}

bool OracleAdmissible(const SpanPlannerConfig& config,
                      const std::vector<SpanPlannerItem>& items,
                      const std::vector<U128>& prefix_vector,
                      const std::vector<U128>& prefix_credit, size_t begin,
                      size_t end) {
    const uint64_t physical_end =
        items[end - 1].tile_offset + items[end - 1].vector_bytes;
    const U128 physical = physical_end - items[begin].tile_offset;
    const U128 vectors = prefix_vector[end] - prefix_vector[begin];
    const bool safein = SafeInAware(config.mode);
    const U128 credits = safein
                             ? prefix_credit[end - 1] - prefix_credit[begin]
                             : 0;
    const U128 rho_num = safein ? config.rho_num : 0;
    const U128 rho_den = safein ? config.rho_den : 1;
    return physical * config.alpha_den * rho_den <=
           config.alpha_num * (vectors * rho_den + credits * rho_num);
}

std::vector<SpanPlannerGroup> OraclePlan(
    const SpanPlannerConfig& config,
    const std::vector<SpanPlannerItem>& items) {
    const size_t n = items.size();
    std::vector<U128> prefix_vector(n + 1, 0);
    std::vector<U128> prefix_credit(n + 1, 0);
    for (size_t i = 0; i < n; ++i) {
        prefix_vector[i + 1] = prefix_vector[i] + items[i].vector_bytes;
        prefix_credit[i + 1] =
            prefix_credit[i] + items[i].safein_internal_credit_bytes;
    }

    std::vector<OracleState> dp(n + 1);
    dp[0].valid = true;
    for (size_t end = 1; end <= n; ++end) {
        OracleState best;
        for (size_t begin = 0; begin < end; ++begin) {
            if (!dp[begin].valid ||
                !OracleAdmissible(config, items, prefix_vector, prefix_credit,
                                  begin, end)) {
                continue;
            }
            const uint64_t physical_end =
                items[end - 1].tile_offset + items[end - 1].vector_bytes;
            const uint64_t group_bytes =
                physical_end - items[begin].tile_offset;
            const OracleState candidate{dp[begin].requests + 1,
                                        dp[begin].bytes + group_bytes, begin,
                                        true};
            if (!best.valid ||
                std::tie(candidate.requests, candidate.bytes,
                         candidate.predecessor) <
                    std::tie(best.requests, best.bytes, best.predecessor)) {
                best = candidate;
            }
        }
        dp[end] = best;
    }

    std::vector<SpanPlannerGroup> groups(dp[n].requests);
    size_t end = n;
    for (size_t slot = groups.size(); slot != 0; --slot) {
        const size_t begin = dp[end].predecessor;
        SpanPlannerGroup& group = groups[slot - 1];
        group.begin = begin;
        group.end = end;
        group.physical_offset = items[begin].tile_offset;
        group.physical_bytes = items[end - 1].tile_offset +
                                   items[end - 1].vector_bytes -
                               items[begin].tile_offset;
        group.vector_bytes =
            static_cast<uint64_t>(prefix_vector[end] - prefix_vector[begin]);
        group.safein_internal_credit_bytes =
            SafeInAware(config.mode) && config.rho_num != 0
                                                  ? static_cast<uint64_t>(
                                                        prefix_credit[end - 1] -
                                                        prefix_credit[begin])
                                                  : 0;
        end = begin;
    }
    return groups;
}

void ExpectSameGroups(const std::vector<SpanPlannerGroup>& actual,
                      const std::vector<SpanPlannerGroup>& expected) {
    ASSERT_EQ(actual.size(), expected.size());
    for (size_t i = 0; i < actual.size(); ++i) {
        EXPECT_EQ(actual[i].begin, expected[i].begin) << "group " << i;
        EXPECT_EQ(actual[i].end, expected[i].end) << "group " << i;
        EXPECT_EQ(actual[i].physical_offset, expected[i].physical_offset)
            << "group " << i;
        EXPECT_EQ(actual[i].physical_bytes, expected[i].physical_bytes)
            << "group " << i;
        EXPECT_EQ(actual[i].vector_bytes, expected[i].vector_bytes)
            << "group " << i;
        EXPECT_EQ(actual[i].safein_internal_credit_bytes,
                  expected[i].safein_internal_credit_bytes)
            << "group " << i;
    }
}

bool RunPlanner(const SpanPlannerConfig& config,
                const std::vector<SpanPlannerItem>& items,
                std::vector<SpanPlannerGroup>* groups,
                SpanPlannerStats* stats = nullptr,
                std::string* error = nullptr) {
    SpanPlannerScratch scratch;
    return PlanSpanRun(config, items.data(), items.size(), &scratch, groups,
                       stats, error);
}

TEST(SpanPlannerTest, GreedyClosesAtFirstFailure) {
    const std::vector<SpanPlannerItem> items = {
        {0, 64, 0}, {120, 64, 0}, {240, 64, 0}};
    SpanPlannerConfig config;
    config.mode = SpanPlannerMode::GV;

    std::vector<SpanPlannerGroup> groups;
    SpanPlannerStats stats;
    ASSERT_TRUE(RunPlanner(config, items, &groups, &stats));
    ASSERT_EQ(groups.size(), 2u);
    EXPECT_EQ(groups[0].begin, 0u);
    EXPECT_EQ(groups[0].end, 2u);
    EXPECT_EQ(groups[0].physical_bytes, 184u);
    EXPECT_EQ(groups[1].begin, 2u);
    EXPECT_EQ(groups[1].end, 3u);
    EXPECT_EQ(stats.group_count, 2u);
    EXPECT_EQ(stats.physical_bytes, 248u);
    EXPECT_EQ(stats.vector_bytes, 192u);
}

TEST(SpanPlannerTest, ExactUsesEarliestPredecessorForObjectiveTie) {
    const std::vector<SpanPlannerItem> items = {
        {0, 64, 0}, {120, 64, 0}, {240, 64, 0}};
    SpanPlannerConfig config;
    config.mode = SpanPlannerMode::GE;

    std::vector<SpanPlannerGroup> groups;
    ASSERT_TRUE(RunPlanner(config, items, &groups));
    ASSERT_EQ(groups.size(), 2u);
    EXPECT_EQ(groups[0].begin, 0u);
    EXPECT_EQ(groups[0].end, 1u);
    EXPECT_EQ(groups[1].begin, 1u);
    EXPECT_EQ(groups[1].end, 3u);
}

TEST(SpanPlannerTest, SafeInInternalCreditCanAdmitSpan) {
    const std::vector<SpanPlannerItem> items = {
        {0, 64, 32}, {150, 64, 0}};
    SpanPlannerConfig config;
    config.mode = SpanPlannerMode::SV;

    std::vector<SpanPlannerGroup> groups;
    SpanPlannerStats stats;
    ASSERT_TRUE(RunPlanner(config, items, &groups, &stats));
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].safein_internal_credit_bytes, 32u);
    EXPECT_EQ(stats.credited_safein_bytes, 32u);
}

TEST(SpanPlannerTest, EndpointSafeInCreditIsExcluded) {
    const std::vector<SpanPlannerItem> items = {
        {0, 64, 0}, {150, 64, 64}};
    for (const SpanPlannerMode mode : {SpanPlannerMode::SV,
                                       SpanPlannerMode::SE}) {
        SpanPlannerConfig config;
        config.mode = mode;
        std::vector<SpanPlannerGroup> groups;
        ASSERT_TRUE(RunPlanner(config, items, &groups));
        EXPECT_EQ(groups.size(), 2u);
        EXPECT_EQ(groups[0].safein_internal_credit_bytes, 0u);
        EXPECT_EQ(groups[1].safein_internal_credit_bytes, 0u);
    }
}

TEST(SpanPlannerTest, ExternalPayloadWithZeroInputCreditIsNotCounted) {
    const std::vector<SpanPlannerItem> items = {
        {0, 64, 0}, {150, 64, 0}};  // First payload is external/cold.
    SpanPlannerConfig config;
    config.mode = SpanPlannerMode::SE;
    std::vector<SpanPlannerGroup> groups;
    ASSERT_TRUE(RunPlanner(config, items, &groups));
    EXPECT_EQ(groups.size(), 2u);
}

TEST(SpanPlannerTest, RhoZeroMatchesVectorOnlyModes) {
    const std::vector<SpanPlannerItem> items = {
        {0, 64, 128}, {150, 64, 64}, {310, 64, 32}, {400, 64, 0}};
    for (const auto& modes : {
             std::pair{SpanPlannerMode::GV, SpanPlannerMode::SV},
             std::pair{SpanPlannerMode::GE, SpanPlannerMode::SE}}) {
        SpanPlannerConfig vector_config;
        vector_config.mode = modes.first;
        SpanPlannerConfig safein_config;
        safein_config.mode = modes.second;
        safein_config.rho_num = 0;
        safein_config.rho_den = 1;

        std::vector<SpanPlannerGroup> vector_groups;
        std::vector<SpanPlannerGroup> safein_groups;
        ASSERT_TRUE(RunPlanner(vector_config, items, &vector_groups));
        ASSERT_TRUE(RunPlanner(safein_config, items, &safein_groups));
        ExpectSameGroups(safein_groups, vector_groups);
    }
}

TEST(SpanPlannerTest, ExactMatchesQuadraticOracleOnRandomSmallRuns) {
    std::mt19937_64 rng(0x51A9E17ULL);
    for (size_t trial = 0; trial < 800; ++trial) {
        const size_t n = 1 + rng() % 14;
        std::vector<SpanPlannerItem> items;
        items.reserve(n);
        uint64_t next_offset = rng() % 8;
        for (size_t i = 0; i < n; ++i) {
            const uint32_t vector_bytes =
                32 + static_cast<uint32_t>(rng() % 65);
            const uint32_t credit = static_cast<uint32_t>((rng() % 6) * 16);
            items.push_back({next_offset, vector_bytes, credit});
            next_offset += vector_bytes + rng() % 129;
        }

        for (const SpanPlannerMode mode : {SpanPlannerMode::GE,
                                           SpanPlannerMode::SE}) {
            SpanPlannerConfig config;
            config.mode = mode;
            switch (trial % 3) {
                case 0:
                    config.rho_num = 0;
                    config.rho_den = 1;
                    break;
                case 1:
                    config.rho_num = 1;
                    config.rho_den = 2;
                    break;
                default:
                    config.rho_num = 1;
                    config.rho_den = 1;
                    break;
            }

            std::vector<SpanPlannerGroup> actual;
            std::string error;
            ASSERT_TRUE(RunPlanner(config, items, &actual, nullptr, &error))
                << "trial=" << trial << " error=" << error;
            const std::vector<SpanPlannerGroup> expected =
                OraclePlan(config, items);
            SCOPED_TRACE("trial=" + std::to_string(trial) +
                         " mode=" + std::to_string(static_cast<int>(mode)));
            ExpectSameGroups(actual, expected);
        }
    }
}

TEST(SpanPlannerTest, ExactWeaklyDominatesMatchedGreedy) {
    std::mt19937_64 rng(0xD01A7EULL);
    for (size_t trial = 0; trial < 400; ++trial) {
        const size_t n = 2 + rng() % 20;
        std::vector<SpanPlannerItem> items;
        uint64_t next_offset = 0;
        for (size_t i = 0; i < n; ++i) {
            const uint32_t vector_bytes = 64;
            items.push_back({next_offset, vector_bytes,
                             static_cast<uint32_t>((rng() % 5) * 16)});
            next_offset += vector_bytes + rng() % 161;
        }

        for (const auto& modes : {
                 std::pair{SpanPlannerMode::GV, SpanPlannerMode::GE},
                 std::pair{SpanPlannerMode::SV, SpanPlannerMode::SE}}) {
            SpanPlannerConfig greedy_config;
            greedy_config.mode = modes.first;
            greedy_config.rho_num = trial % 2 == 0 ? 1 : 1;
            greedy_config.rho_den = trial % 2 == 0 ? 2 : 1;
            SpanPlannerConfig exact_config = greedy_config;
            exact_config.mode = modes.second;

            std::vector<SpanPlannerGroup> greedy;
            std::vector<SpanPlannerGroup> exact;
            SpanPlannerStats greedy_stats;
            SpanPlannerStats exact_stats;
            ASSERT_TRUE(
                RunPlanner(greedy_config, items, &greedy, &greedy_stats));
            ASSERT_TRUE(
                RunPlanner(exact_config, items, &exact, &exact_stats));
            EXPECT_LE(std::tie(exact_stats.group_count,
                               exact_stats.physical_bytes),
                      std::tie(greedy_stats.group_count,
                               greedy_stats.physical_bytes));
        }
    }
}

TEST(SpanPlannerTest, ReusableScratchSupportsChangingRunSizes) {
    SpanPlannerScratch scratch;
    scratch.Reserve(64);
    SpanPlannerConfig config;
    config.mode = SpanPlannerMode::GE;
    std::vector<SpanPlannerGroup> groups;
    SpanPlannerStats stats;

    const std::vector<SpanPlannerItem> large = {
        {0, 64, 0}, {100, 64, 0}, {200, 64, 0}, {300, 64, 0}};
    ASSERT_TRUE(PlanSpanRun(config, large.data(), large.size(), &scratch,
                            &groups, &stats));
    EXPECT_EQ(stats.item_count, 4u);
    EXPECT_EQ(stats.workspace_growths, 0u);

    const std::vector<SpanPlannerItem> small = {{7, 32, 0}};
    ASSERT_TRUE(PlanSpanRun(config, small.data(), small.size(), &scratch,
                            &groups, &stats));
    ASSERT_EQ(groups.size(), 1u);
    EXPECT_EQ(groups[0].physical_offset, 7u);

    ASSERT_TRUE(PlanSpanRun(config, nullptr, 0, &scratch, &groups, &stats));
    EXPECT_TRUE(groups.empty());
    EXPECT_EQ(stats.item_count, 0u);
}

TEST(SpanPlannerTest, RejectsInvalidContractsAndInput) {
    SpanPlannerScratch scratch;
    std::vector<SpanPlannerGroup> groups;
    SpanPlannerStats stats;
    std::string error;
    const SpanPlannerItem valid{0, 64, 0};
    SpanPlannerConfig config;

    config.alpha_den = 0;
    EXPECT_FALSE(PlanSpanRun(config, &valid, 1, &scratch, &groups, &stats,
                             &error));
    config = {};
    config.alpha_num = 1;
    config.alpha_den = 2;
    EXPECT_FALSE(PlanSpanRun(config, &valid, 1, &scratch, &groups, &stats,
                             &error));
    config = {};
    config.rho_den = 0;
    EXPECT_FALSE(PlanSpanRun(config, &valid, 1, &scratch, &groups, &stats,
                             &error));
    config = {};
    config.rho_num = 2;
    config.rho_den = 1;
    EXPECT_FALSE(PlanSpanRun(config, &valid, 1, &scratch, &groups, &stats,
                             &error));

    config = {};
    const SpanPlannerItem zero_vector{0, 0, 0};
    EXPECT_FALSE(PlanSpanRun(config, &zero_vector, 1, &scratch, &groups,
                             &stats, &error));
    const SpanPlannerItem unordered[] = {{100, 64, 0}, {50, 64, 0}};
    EXPECT_FALSE(PlanSpanRun(config, unordered, 2, &scratch, &groups, &stats,
                             &error));
    const SpanPlannerItem overflowing{
        std::numeric_limits<uint64_t>::max() - 31, 64, 0};
    EXPECT_FALSE(PlanSpanRun(config, &overflowing, 1, &scratch, &groups,
                             &stats, &error));
    EXPECT_FALSE(PlanSpanRun(config, nullptr, 1, &scratch, &groups, &stats,
                             &error));
    EXPECT_FALSE(PlanSpanRun(config, &valid, 1, nullptr, &groups, &stats,
                             &error));
    EXPECT_FALSE(PlanSpanRun(config, &valid, 1, &scratch, nullptr, &stats,
                             &error));

    config = {};
    config.mode = SpanPlannerMode::SE;
    config.alpha_num = std::numeric_limits<uint64_t>::max();
    config.alpha_den = std::numeric_limits<uint64_t>::max();
    config.rho_num = 1;
    config.rho_den = std::numeric_limits<uint64_t>::max();
    const SpanPlannerItem arithmetic_overflow{1, 1, 0};
    EXPECT_FALSE(PlanSpanRun(config, &arithmetic_overflow, 1, &scratch,
                             &groups, &stats, &error));
}

}  // namespace
}  // namespace query
}  // namespace vdb
