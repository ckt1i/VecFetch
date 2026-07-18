#include "vdb/query/span_planner.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace vdb {
namespace query {
namespace {

using U128 = unsigned __int128;

constexpr U128 kU128Max = ~static_cast<U128>(0);
constexpr size_t kBoundedDirectDpItems = 8;

bool CheckedAdd(U128 lhs, U128 rhs, U128* out) {
    if (lhs > kU128Max - rhs) return false;
    *out = lhs + rhs;
    return true;
}

bool CheckedMul(U128 lhs, U128 rhs, U128* out) {
    if (lhs != 0 && rhs > kU128Max / lhs) return false;
    *out = lhs * rhs;
    return true;
}

struct SignedMagnitude {
    U128 magnitude = 0;
    bool negative = false;
};

SignedMagnitude Difference(U128 lhs, U128 rhs) {
    if (lhs >= rhs) return {lhs - rhs, false};
    return {rhs - lhs, true};
}

bool SignedLess(const SignedMagnitude& lhs, const SignedMagnitude& rhs) {
    if (lhs.negative != rhs.negative) return lhs.negative;
    if (!lhs.negative) return lhs.magnitude < rhs.magnitude;
    return lhs.magnitude > rhs.magnitude;
}

bool SignedEqual(const SignedMagnitude& lhs, const SignedMagnitude& rhs) {
    return lhs.negative == rhs.negative && lhs.magnitude == rhs.magnitude;
}

struct DpState {
    uint64_t requests = 0;
    uint64_t physical_bytes = 0;
    bool valid = false;
};

// A start-boundary candidate is ordered by the objective it induces for any
// fixed endpoint: request count, then dp_bytes - start_offset, then the
// earliest predecessor. The endpoint's physical end is a common additive
// constant and therefore need not be stored in the Fenwick tree.
struct StartCandidate {
    uint64_t requests = 0;
    SignedMagnitude adjusted_bytes;
    size_t predecessor = 0;
    bool valid = false;
};

bool CandidateLess(const StartCandidate& lhs, const StartCandidate& rhs) {
    if (!lhs.valid) return false;
    if (!rhs.valid) return true;
    if (lhs.requests != rhs.requests) return lhs.requests < rhs.requests;
    if (!SignedEqual(lhs.adjusted_bytes, rhs.adjusted_bytes)) {
        return SignedLess(lhs.adjusted_bytes, rhs.adjusted_bytes);
    }
    return lhs.predecessor < rhs.predecessor;
}

bool IsSafeInAware(SpanPlannerMode mode) {
    return mode == SpanPlannerMode::SV || mode == SpanPlannerMode::SE;
}

bool IsExact(SpanPlannerMode mode) {
    return mode == SpanPlannerMode::GE || mode == SpanPlannerMode::SE;
}

bool IsValidMode(SpanPlannerMode mode) {
    switch (mode) {
        case SpanPlannerMode::GV:
        case SpanPlannerMode::SV:
        case SpanPlannerMode::GE:
        case SpanPlannerMode::SE:
            return true;
    }
    return false;
}

bool Fail(std::vector<SpanPlannerGroup>* groups, SpanPlannerStats* stats,
          std::string* error, const char* message) {
    if (groups != nullptr) groups->clear();
    if (stats != nullptr) *stats = {};
    if (error != nullptr) *error = message;
    return false;
}

}  // namespace

struct SpanPlannerScratch::Impl {
    std::vector<U128> prefix_vector;
    std::vector<U128> prefix_credit;
    std::vector<SignedMagnitude> start_keys;
    std::vector<SignedMagnitude> endpoint_keys;
    std::vector<SignedMagnitude> coordinates;
    std::vector<DpState> dp;
    std::vector<size_t> predecessor;
    std::vector<StartCandidate> fenwick;
    size_t reserved_items = 0;

    uint64_t Reserve(size_t n) {
        if (n <= reserved_items) return 0;
        uint64_t growths = 0;
        const auto reserve = [&growths](auto* values, size_t capacity) {
            const size_t before = values->capacity();
            values->reserve(capacity);
            growths += values->capacity() != before ? 1 : 0;
        };
        reserve(&prefix_vector, n + 1);
        reserve(&prefix_credit, n + 1);
        reserve(&start_keys, n);
        reserve(&endpoint_keys, n);
        reserve(&coordinates, n);
        reserve(&dp, n + 1);
        reserve(&predecessor, n + 1);
        reserve(&fenwick, n + 1);
        reserved_items = n;
        return growths;
    }
};

SpanPlannerScratch::SpanPlannerScratch() : impl_(new Impl()) {}
SpanPlannerScratch::~SpanPlannerScratch() = default;
SpanPlannerScratch::SpanPlannerScratch(SpanPlannerScratch&&) noexcept = default;
SpanPlannerScratch& SpanPlannerScratch::operator=(
    SpanPlannerScratch&&) noexcept = default;

void SpanPlannerScratch::Reserve(size_t item_count) {
    (void)impl_->Reserve(item_count);
}

bool PlanSpanRun(const SpanPlannerConfig& config,
                 const SpanPlannerItem* items, size_t item_count,
                 SpanPlannerScratch* scratch,
                 std::vector<SpanPlannerGroup>* groups,
                 SpanPlannerStats* stats, std::string* error) {
    if (groups == nullptr) {
        return Fail(nullptr, stats, error, "groups must not be null");
    }
    groups->clear();
    if (stats != nullptr) *stats = {};
    if (error != nullptr) error->clear();

    if (scratch == nullptr || scratch->impl_ == nullptr) {
        return Fail(groups, stats, error, "scratch must not be null");
    }
    if (item_count != 0 && items == nullptr) {
        return Fail(groups, stats, error,
                    "items must not be null for non-empty input");
    }
    if (!IsValidMode(config.mode)) {
        return Fail(groups, stats, error, "invalid planner mode");
    }
    if (config.alpha_den == 0 || config.alpha_num < config.alpha_den) {
        return Fail(groups, stats, error,
                    "alpha must be a rational value greater than or equal to one");
    }
    if (config.rho_den == 0 || config.rho_num > config.rho_den) {
        return Fail(groups, stats, error,
                    "rho must be a rational value in the closed interval [0, 1]");
    }
    if (item_count == 0) return true;

    SpanPlannerScratch::Impl& work = *scratch->impl_;
    const uint64_t workspace_growths = work.Reserve(item_count);
    work.prefix_vector.resize(item_count + 1);
    work.prefix_credit.resize(item_count + 1);
    work.prefix_vector[0] = 0;
    work.prefix_credit[0] = 0;

    uint64_t previous_offset = 0;
    uint64_t previous_end = 0;
    for (size_t i = 0; i < item_count; ++i) {
        const SpanPlannerItem& item = items[i];
        if (item.vector_bytes == 0) {
            return Fail(groups, stats, error,
                        "every item must contain at least one vector byte");
        }
        if (i != 0 && item.tile_offset < previous_offset) {
            return Fail(groups, stats, error,
                        "items must be ordered by nondecreasing tile offset");
        }
        if (item.tile_offset >
            std::numeric_limits<uint64_t>::max() - item.vector_bytes) {
            return Fail(groups, stats, error, "item physical end overflows uint64");
        }
        const uint64_t item_end = item.tile_offset + item.vector_bytes;
        if (i != 0 && item_end < previous_end) {
            return Fail(groups, stats, error,
                        "item physical ends must be nondecreasing");
        }

        U128 next = 0;
        if (!CheckedAdd(work.prefix_vector[i], item.vector_bytes, &next) ||
            next > std::numeric_limits<uint64_t>::max()) {
            return Fail(groups, stats, error, "total vector bytes overflow uint64");
        }
        work.prefix_vector[i + 1] = next;
        if (!CheckedAdd(work.prefix_credit[i],
                        item.safein_internal_credit_bytes, &next) ||
            next > std::numeric_limits<uint64_t>::max()) {
            return Fail(groups, stats, error, "total SafeIn credit overflows uint64");
        }
        work.prefix_credit[i + 1] = next;
        previous_offset = item.tile_offset;
        previous_end = item_end;
    }

    const bool safein_aware = IsSafeInAware(config.mode);
    const uint64_t rho_num = safein_aware ? config.rho_num : 0;
    const uint64_t rho_den = safein_aware ? config.rho_den : 1;

    U128 amplification_denominator = 0;
    if (!CheckedMul(config.alpha_den, rho_den,
                    &amplification_denominator)) {
        return Fail(groups, stats, error,
                    "alpha/rho denominator product overflows uint128");
    }

    auto group_values = [&](size_t begin, size_t end, uint64_t* physical,
                            uint64_t* vectors, uint64_t* credits) -> bool {
        const uint64_t physical_end =
            items[end - 1].tile_offset + items[end - 1].vector_bytes;
        *physical = physical_end - items[begin].tile_offset;
        const U128 vector_sum =
            work.prefix_vector[end] - work.prefix_vector[begin];
        const U128 credit_sum = safein_aware && rho_num != 0
                                    ? work.prefix_credit[end - 1] -
                                          work.prefix_credit[begin]
                                    : 0;
        if (vector_sum > std::numeric_limits<uint64_t>::max() ||
            credit_sum > std::numeric_limits<uint64_t>::max()) {
            return false;
        }
        *vectors = static_cast<uint64_t>(vector_sum);
        *credits = static_cast<uint64_t>(credit_sum);
        return true;
    };

    auto admissible = [&](size_t begin, size_t end, bool* overflow) -> bool {
        uint64_t physical = 0;
        uint64_t vectors = 0;
        uint64_t credits = 0;
        if (!group_values(begin, end, &physical, &vectors, &credits)) {
            *overflow = true;
            return false;
        }

        U128 vector_term = 0;
        U128 credit_term = 0;
        U128 effective = 0;
        U128 lhs = 0;
        U128 rhs = 0;
        if (!CheckedMul(vectors, rho_den, &vector_term) ||
            !CheckedMul(credits, rho_num, &credit_term) ||
            !CheckedAdd(vector_term, credit_term, &effective) ||
            !CheckedMul(physical, amplification_denominator, &lhs) ||
            !CheckedMul(config.alpha_num, effective, &rhs)) {
            *overflow = true;
            return false;
        }
        return lhs <= rhs;
    };

    auto fill_group = [&](size_t begin, size_t end,
                          SpanPlannerGroup* group) -> bool {
        uint64_t physical = 0;
        uint64_t vectors = 0;
        uint64_t credits = 0;
        if (!group_values(begin, end, &physical, &vectors, &credits)) {
            return false;
        }
        group->begin = begin;
        group->end = end;
        group->physical_offset = items[begin].tile_offset;
        group->physical_bytes = physical;
        group->vector_bytes = vectors;
        group->safein_internal_credit_bytes = credits;
        return true;
    };

    uint64_t admission_checks = 0;
    uint64_t fenwick_queries = 0;
    uint64_t fenwick_updates = 0;

    if (!IsExact(config.mode)) {
        groups->reserve(std::max(groups->capacity(), item_count));
        size_t begin = 0;
        for (size_t end = 1; end <= item_count; ++end) {
            ++admission_checks;
            bool overflow = false;
            if (admissible(begin, end, &overflow)) continue;
            if (overflow) {
                return Fail(groups, stats, error,
                            "greedy admission arithmetic overflows uint128");
            }
            if (end == begin + 1) {
                return Fail(groups, stats, error,
                            "singleton group unexpectedly violates admission");
            }

            // alpha >= 1 guarantees that the newly failing endpoint is a
            // valid singleton, so close at the first failure and restart.
            SpanPlannerGroup group;
            if (!fill_group(begin, end - 1, &group)) {
                return Fail(groups, stats, error,
                            "greedy group statistics overflow uint64");
            }
            groups->push_back(group);
            begin = end - 1;
        }
        SpanPlannerGroup group;
        if (!fill_group(begin, item_count, &group)) {
            return Fail(groups, stats, error,
                        "greedy group statistics overflow uint64");
        }
        groups->push_back(group);
    } else if (item_count <= kBoundedDirectDpItems) {
        // Real traces are dominated by very short runs. A bounded direct DP
        // avoids coordinate compression and Fenwick setup for those runs while
        // preserving the exact lexicographic objective. The bound is constant,
        // so the planner remains O(n log n) asymptotically.
        work.dp.assign(item_count + 1, {});
        work.predecessor.assign(item_count + 1, 0);
        work.dp[0] = {0, 0, true};

        // Keep the bounded path's overflow contract identical to the Fenwick
        // transformation even though it does not materialize those keys.
        const auto exact_key_is_representable =
            [&](U128 vectors, U128 credits, uint64_t offset) {
                U128 vector_term = 0;
                U128 credit_term = 0;
                U128 weighted = 0;
                U128 offset_term = 0;
                U128 prefix_term = 0;
                return CheckedMul(vectors, rho_den, &vector_term) &&
                       CheckedMul(credits, rho_num, &credit_term) &&
                       CheckedAdd(vector_term, credit_term, &weighted) &&
                       CheckedMul(offset, amplification_denominator,
                                  &offset_term) &&
                       CheckedMul(config.alpha_num, weighted, &prefix_term);
            };
        for (size_t i = 0; i < item_count; ++i) {
            const uint64_t physical_end =
                items[i].tile_offset + items[i].vector_bytes;
            if (!exact_key_is_representable(work.prefix_vector[i],
                                            work.prefix_credit[i],
                                            items[i].tile_offset) ||
                !exact_key_is_representable(work.prefix_vector[i + 1],
                                            work.prefix_credit[i],
                                            physical_end)) {
                return Fail(
                    groups, stats, error,
                    "bounded exact key arithmetic overflows uint128");
            }
        }

        for (size_t end = 1; end <= item_count; ++end) {
            DpState best;
            size_t best_begin = 0;
            for (size_t begin = 0; begin < end; ++begin) {
                ++admission_checks;
                bool overflow = false;
                if (!admissible(begin, end, &overflow)) {
                    if (overflow) {
                        return Fail(
                            groups, stats, error,
                            "bounded exact admission arithmetic overflows uint128");
                    }
                    continue;
                }

                const uint64_t physical_end =
                    items[end - 1].tile_offset + items[end - 1].vector_bytes;
                const uint64_t group_bytes =
                    physical_end - items[begin].tile_offset;
                if (!work.dp[begin].valid ||
                    work.dp[begin].physical_bytes >
                        std::numeric_limits<uint64_t>::max() - group_bytes) {
                    return Fail(
                        groups, stats, error,
                        "bounded exact objective bytes overflow uint64");
                }
                const DpState candidate{
                    work.dp[begin].requests + 1,
                    work.dp[begin].physical_bytes + group_bytes, true};
                if (!best.valid || candidate.requests < best.requests ||
                    (candidate.requests == best.requests &&
                     (candidate.physical_bytes < best.physical_bytes ||
                      (candidate.physical_bytes == best.physical_bytes &&
                       begin < best_begin)))) {
                    best = candidate;
                    best_begin = begin;
                }
            }
            if (!best.valid) {
                return Fail(
                    groups, stats, error,
                    "bounded exact planner found no admissible predecessor");
            }
            work.dp[end] = best;
            work.predecessor[end] = best_begin;
        }

        groups->resize(static_cast<size_t>(work.dp[item_count].requests));
        size_t end = item_count;
        for (size_t slot = groups->size(); slot != 0; --slot) {
            const size_t begin = work.predecessor[end];
            if (begin >= end ||
                !fill_group(begin, end, &(*groups)[slot - 1])) {
                return Fail(groups, stats, error,
                            "bounded exact predecessor chain is invalid");
            }
            end = begin;
        }
        if (end != 0) {
            return Fail(
                groups, stats, error,
                "bounded exact predecessor chain does not reach the run origin");
        }
    } else {
        work.start_keys.resize(item_count);
        work.endpoint_keys.resize(item_count);
        work.coordinates.resize(item_count);

        auto weighted_prefix = [&](U128 vectors, U128 credits,
                                   U128* out) -> bool {
            U128 vector_term = 0;
            U128 credit_term = 0;
            return CheckedMul(vectors, rho_den, &vector_term) &&
                   CheckedMul(credits, rho_num, &credit_term) &&
                   CheckedAdd(vector_term, credit_term, out);
        };

        for (size_t i = 0; i < item_count; ++i) {
            U128 weighted = 0;
            U128 offset_term = 0;
            U128 prefix_term = 0;
            if (!weighted_prefix(work.prefix_vector[i],
                                 work.prefix_credit[i], &weighted) ||
                !CheckedMul(items[i].tile_offset,
                            amplification_denominator, &offset_term) ||
                !CheckedMul(config.alpha_num, weighted, &prefix_term)) {
                return Fail(groups, stats, error,
                            "exact start-key arithmetic overflows uint128");
            }
            work.start_keys[i] = Difference(offset_term, prefix_term);
            work.coordinates[i] = work.start_keys[i];
        }

        for (size_t end = 1; end <= item_count; ++end) {
            const uint64_t physical_end =
                items[end - 1].tile_offset + items[end - 1].vector_bytes;
            U128 weighted = 0;
            U128 offset_term = 0;
            U128 prefix_term = 0;
            if (!weighted_prefix(work.prefix_vector[end],
                                 work.prefix_credit[end - 1], &weighted) ||
                !CheckedMul(physical_end, amplification_denominator,
                            &offset_term) ||
                !CheckedMul(config.alpha_num, weighted, &prefix_term)) {
                return Fail(groups, stats, error,
                            "exact endpoint-key arithmetic overflows uint128");
            }
            work.endpoint_keys[end - 1] = Difference(offset_term, prefix_term);
        }

        const auto key_less = [](const SignedMagnitude& lhs,
                                 const SignedMagnitude& rhs) {
            return SignedLess(lhs, rhs);
        };
        std::sort(work.coordinates.begin(), work.coordinates.end(), key_less);
        work.coordinates.erase(
            std::unique(work.coordinates.begin(), work.coordinates.end(),
                        [](const SignedMagnitude& lhs,
                           const SignedMagnitude& rhs) {
                            return SignedEqual(lhs, rhs);
                        }),
            work.coordinates.end());

        work.dp.assign(item_count + 1, {});
        work.predecessor.assign(item_count + 1, 0);
        work.fenwick.assign(work.coordinates.size() + 1, {});
        work.dp[0] = {0, 0, true};

        auto fenwick_update = [&](const SignedMagnitude& key,
                                  const StartCandidate& candidate) {
            const size_t coordinate = static_cast<size_t>(std::lower_bound(
                work.coordinates.begin(), work.coordinates.end(), key,
                key_less) - work.coordinates.begin());
            size_t position = work.coordinates.size() - coordinate;
            while (position < work.fenwick.size()) {
                if (CandidateLess(candidate, work.fenwick[position])) {
                    work.fenwick[position] = candidate;
                }
                position += position & (~position + 1);
            }
            ++fenwick_updates;
        };

        auto fenwick_query = [&](const SignedMagnitude& threshold) {
            const size_t lower = static_cast<size_t>(std::lower_bound(
                work.coordinates.begin(), work.coordinates.end(), threshold,
                key_less) - work.coordinates.begin());
            size_t position = work.coordinates.size() - lower;
            StartCandidate best;
            while (position != 0) {
                if (CandidateLess(work.fenwick[position], best)) {
                    best = work.fenwick[position];
                }
                position -= position & (~position + 1);
            }
            ++fenwick_queries;
            return best;
        };

        for (size_t end = 1; end <= item_count; ++end) {
            const size_t start = end - 1;
            const DpState& start_state = work.dp[start];
            StartCandidate candidate;
            candidate.requests = start_state.requests;
            candidate.adjusted_bytes = Difference(
                start_state.physical_bytes, items[start].tile_offset);
            candidate.predecessor = start;
            candidate.valid = start_state.valid;
            fenwick_update(work.start_keys[start], candidate);

            ++admission_checks;
            const StartCandidate best =
                fenwick_query(work.endpoint_keys[end - 1]);
            if (!best.valid) {
                return Fail(groups, stats, error,
                            "exact planner found no admissible predecessor");
            }
            const size_t begin = best.predecessor;
            const uint64_t physical_end =
                items[end - 1].tile_offset + items[end - 1].vector_bytes;
            const uint64_t group_bytes =
                physical_end - items[begin].tile_offset;
            if (work.dp[begin].physical_bytes >
                std::numeric_limits<uint64_t>::max() - group_bytes) {
                return Fail(groups, stats, error,
                            "exact objective bytes overflow uint64");
            }
            work.dp[end] = {work.dp[begin].requests + 1,
                            work.dp[begin].physical_bytes + group_bytes, true};
            work.predecessor[end] = begin;
        }

        if (work.dp[item_count].requests > item_count) {
            return Fail(groups, stats, error,
                        "exact planner produced an invalid group count");
        }
        groups->resize(static_cast<size_t>(work.dp[item_count].requests));
        size_t end = item_count;
        for (size_t slot = groups->size(); slot != 0; --slot) {
            const size_t begin = work.predecessor[end];
            if (begin >= end || !fill_group(begin, end, &(*groups)[slot - 1])) {
                return Fail(groups, stats, error,
                            "exact predecessor chain is invalid");
            }
            end = begin;
        }
        if (end != 0) {
            return Fail(groups, stats, error,
                        "exact predecessor chain does not reach the run origin");
        }
    }

    SpanPlannerStats result;
    result.item_count = item_count;
    result.group_count = groups->size();
    result.admission_checks = admission_checks;
    result.fenwick_queries = fenwick_queries;
    result.fenwick_updates = fenwick_updates;
    result.workspace_growths = workspace_growths;
    result.vector_bytes =
        static_cast<uint64_t>(work.prefix_vector[item_count]);
    for (const SpanPlannerGroup& group : *groups) {
        if (result.physical_bytes >
                std::numeric_limits<uint64_t>::max() - group.physical_bytes ||
            result.credited_safein_bytes >
                std::numeric_limits<uint64_t>::max() -
                    group.safein_internal_credit_bytes) {
            return Fail(groups, stats, error, "planner statistics overflow uint64");
        }
        result.physical_bytes += group.physical_bytes;
        result.credited_safein_bytes +=
            group.safein_internal_credit_bytes;
    }
    if (stats != nullptr) *stats = result;
    return true;
}

}  // namespace query
}  // namespace vdb
