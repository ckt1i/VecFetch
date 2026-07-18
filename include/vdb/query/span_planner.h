#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vdb {
namespace query {

// The two letters describe the admission model and the solver:
// G/S: vector-only or SafeIn-aware; V/E: greedy or exact.
enum class SpanPlannerMode : uint8_t {
    GV = 0,
    SV = 1,
    GE = 2,
    SE = 3,
};

struct SpanPlannerConfig {
    SpanPlannerMode mode = SpanPlannerMode::GV;

    // Exact rational contracts. A group is admissible iff
    //
    //   B <= alpha * (V + rho * S_internal).
    //
    // Vector-only modes ignore rho and all SafeIn credits. SafeIn-aware modes
    // support rho in [0, 1], including the experiment settings 0, 1/2, and 1.
    uint64_t alpha_num = 3;
    uint64_t alpha_den = 2;
    uint64_t rho_num = 1;
    uint64_t rho_den = 1;
};

struct SpanPlannerItem {
    // Offset relative to the beginning of the current tile/run. Callers must
    // provide items in nondecreasing physical order.
    uint64_t tile_offset = 0;
    uint32_t vector_bytes = 0;

    // Bytes that are both resident-inline and reusable if this item becomes an
    // internal member of a span. External/cold payloads must be passed as zero.
    // The planner automatically excludes the group endpoint's credit.
    uint32_t safein_internal_credit_bytes = 0;
};

struct SpanPlannerGroup {
    size_t begin = 0;
    size_t end = 0;  // Half-open item range [begin, end).
    uint64_t physical_offset = 0;
    uint64_t physical_bytes = 0;
    uint64_t vector_bytes = 0;
    uint64_t safein_internal_credit_bytes = 0;
};

struct SpanPlannerStats {
    size_t item_count = 0;
    size_t group_count = 0;
    uint64_t physical_bytes = 0;
    uint64_t vector_bytes = 0;
    uint64_t credited_safein_bytes = 0;
    uint64_t admission_checks = 0;
    uint64_t fenwick_queries = 0;
    uint64_t fenwick_updates = 0;
    uint64_t workspace_growths = 0;
};

// Reusable storage for the exact planner. Construct one per scheduler/query
// context and reuse it across runs to avoid planner-internal per-call heap
// allocation after the high-water mark has been reached.
class SpanPlannerScratch {
 public:
    SpanPlannerScratch();
    ~SpanPlannerScratch();
    SpanPlannerScratch(SpanPlannerScratch&&) noexcept;
    SpanPlannerScratch& operator=(SpanPlannerScratch&&) noexcept;

    SpanPlannerScratch(const SpanPlannerScratch&) = delete;
    SpanPlannerScratch& operator=(const SpanPlannerScratch&) = delete;

    void Reserve(size_t item_count);

 private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    friend bool PlanSpanRun(const SpanPlannerConfig&, const SpanPlannerItem*,
                            size_t, SpanPlannerScratch*,
                            std::vector<SpanPlannerGroup>*, SpanPlannerStats*,
                            std::string*);
};

// Plans one already-ordered tile/run. Returns false for an invalid contract or
// arithmetic overflow and leaves a diagnostic in error when it is non-null.
// Empty input is valid. groups and scratch are required; stats is optional.
[[nodiscard]] bool PlanSpanRun(const SpanPlannerConfig& config,
                               const SpanPlannerItem* items,
                               size_t item_count,
                               SpanPlannerScratch* scratch,
                               std::vector<SpanPlannerGroup>* groups,
                               SpanPlannerStats* stats,
                               std::string* error = nullptr);

}  // namespace query
}  // namespace vdb
