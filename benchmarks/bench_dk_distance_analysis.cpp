/// bench_dk_distance_analysis.cpp — Offline RabitQ-space distance distribution
/// analysis for separating d_k strictness from probe/top-k coverage.
///
/// Usage:
///   bench_dk_distance_analysis --input /path/to/per_candidate.csv
///       [--outdir ./dk_distance_output] [--topk 10]

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

namespace {

struct QueryStats {
    uint32_t query_id = 0;
    uint64_t num_candidates = 0;
    uint64_t num_s2_candidates = 0;
    uint64_t true_topk_in_probe = 0;
    uint64_t count_s2_lt_dk = 0;
    uint64_t count_true_topk_s2_lt_dk = 0;
    uint64_t count_s2_safein_current = 0;
    uint64_t count_true_topk_s2_safein_current = 0;
    double dk_static = 0.0;
    std::vector<double> s2_dists;
    std::vector<double> true_topk_gap_s2;
};

struct GlobalSummary {
    uint64_t num_queries = 0;
    uint64_t num_candidates = 0;
    uint64_t num_s2_candidates = 0;
    uint64_t true_topk_in_probe = 0;
    uint64_t count_s2_lt_dk = 0;
    uint64_t count_true_topk_s2_lt_dk = 0;
    uint64_t count_s2_safein_current = 0;
    uint64_t count_true_topk_s2_safein_current = 0;
    uint64_t queries_with_any_true_topk = 0;
    uint64_t queries_with_ge_1_true_topk = 0;
    uint64_t queries_with_ge_2_true_topk = 0;
    uint64_t invariant_violations = 0;
};

static std::string GetArg(int argc, char* argv[], const char* name,
                          const std::string& def) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return def;
}

static int GetIntArg(int argc, char* argv[], const char* name, int def) {
    const std::string s = GetArg(argc, argv, name, "");
    return s.empty() ? def : std::atoi(s.c_str());
}

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

static std::vector<std::string> SplitCsvLine(const std::string& line) {
    std::vector<std::string> cols;
    std::string cur;
    for (char c : line) {
        if (c == ',') {
            cols.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    cols.push_back(cur);
    return cols;
}

static double ParseDoubleOr(const std::string& s, double def) {
    if (s.empty()) return def;
    double out = def;
    auto first = s.data();
    auto last = s.data() + s.size();
    auto [ptr, ec] = std::from_chars(first, last, out);
    if (ec == std::errc()) return out;
    char* end = nullptr;
    out = std::strtod(s.c_str(), &end);
    return (end != s.c_str()) ? out : def;
}

static uint32_t ParseUint32Or(const std::string& s, uint32_t def) {
    if (s.empty()) return def;
    uint32_t out = def;
    auto [ptr, ec] = std::from_chars(s.data(), s.data() + s.size(), out);
    return (ec == std::errc()) ? out : def;
}

static bool ParseBool01(const std::string& s) {
    return !s.empty() && s != "0";
}

static double Percentile(std::vector<double> values, double p) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const double idx = p * static_cast<double>(values.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(idx));
    const size_t hi = static_cast<size_t>(std::ceil(idx));
    if (lo == hi) return values[lo];
    const double frac = idx - static_cast<double>(lo);
    return values[lo] * (1.0 - frac) + values[hi] * frac;
}

static double Rate(uint64_t num, uint64_t denom) {
    return denom == 0 ? 0.0 : static_cast<double>(num) / static_cast<double>(denom);
}

static double DkPercentile(std::vector<double> values, double dk) {
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    const auto it = std::upper_bound(values.begin(), values.end(), dk);
    return static_cast<double>(std::distance(values.begin(), it)) /
           static_cast<double>(values.size());
}

static double MaxOrZero(const std::vector<double>& values) {
    if (values.empty()) return 0.0;
    return *std::max_element(values.begin(), values.end());
}

static void WriteMetricJson(std::ofstream& out,
                            const char* name,
                            const std::vector<double>& values,
                            bool comma) {
    out << "    \"" << name << "\": {\n";
    out << "      \"p50\": " << Percentile(values, 0.50) << ",\n";
    out << "      \"p90\": " << Percentile(values, 0.90) << ",\n";
    out << "      \"p95\": " << Percentile(values, 0.95) << ",\n";
    out << "      \"max\": " << (values.empty() ? 0.0 : *std::max_element(values.begin(), values.end())) << "\n";
    out << "    }" << (comma ? "," : "") << "\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string input = GetArg(argc, argv, "--input", "");
    const std::string outdir = GetArg(argc, argv, "--outdir", "./dk_distance_output");
    const uint32_t topk = static_cast<uint32_t>(GetIntArg(argc, argv, "--topk", 10));

    if (input.empty()) {
        std::fprintf(stderr,
                     "Usage: bench_dk_distance_analysis --input <per_candidate.csv> "
                     "[--outdir ./dk_distance_output] [--topk 10]\n");
        return 1;
    }

    fs::create_directories(outdir);
    Log("=== d_k Distance Analysis ===\n");
    Log("Input:  %s\n", input.c_str());
    Log("Outdir: %s\n", outdir.c_str());

    std::ifstream f(input);
    if (!f.is_open()) {
        std::fprintf(stderr, "Failed to open %s\n", input.c_str());
        return 1;
    }

    std::string header_line;
    if (!std::getline(f, header_line)) {
        std::fprintf(stderr, "Empty input CSV: %s\n", input.c_str());
        return 1;
    }
    std::vector<std::string> header = SplitCsvLine(header_line);
    std::unordered_map<std::string, size_t> col;
    for (size_t i = 0; i < header.size(); ++i) col[header[i]] = i;

    const char* required[] = {
        "query_id", "is_true_topk", "est_dist_s2", "margin_s2_current", "d_k_static"
    };
    for (const char* key : required) {
        if (!col.count(key)) {
            std::fprintf(stderr, "Missing required column: %s\n", key);
            return 1;
        }
    }

    std::unordered_map<uint32_t, QueryStats> by_query;
    std::string line;
    while (std::getline(f, line)) {
        std::vector<std::string> fields = SplitCsvLine(line);
        if (fields.size() < header.size()) fields.resize(header.size());
        const uint32_t qid = ParseUint32Or(fields[col["query_id"]], 0);
        QueryStats& st = by_query[qid];
        st.query_id = qid;
        ++st.num_candidates;
        const bool is_true_topk = ParseBool01(fields[col["is_true_topk"]]);
        if (is_true_topk) ++st.true_topk_in_probe;
        if (fields[col["est_dist_s2"]].empty()) continue;

        const double s2 = ParseDoubleOr(fields[col["est_dist_s2"]], 0.0);
        const double margin_s2 = ParseDoubleOr(fields[col["margin_s2_current"]], 0.0);
        const double dk = ParseDoubleOr(fields[col["d_k_static"]], 0.0);
        st.dk_static = dk;
        ++st.num_s2_candidates;
        st.s2_dists.push_back(s2);
        if (s2 < dk) {
            ++st.count_s2_lt_dk;
            if (is_true_topk) ++st.count_true_topk_s2_lt_dk;
        }
        if (s2 < dk - margin_s2) {
            ++st.count_s2_safein_current;
            if (is_true_topk) ++st.count_true_topk_s2_safein_current;
        }
        if (is_true_topk) {
            st.true_topk_gap_s2.push_back(dk - s2);
        }
    }

    std::vector<QueryStats*> queries;
    queries.reserve(by_query.size());
    for (auto& kv : by_query) queries.push_back(&kv.second);
    std::sort(queries.begin(), queries.end(),
              [](const QueryStats* a, const QueryStats* b) {
                  return a->query_id < b->query_id;
              });

    GlobalSummary g;
    g.num_queries = queries.size();
    std::vector<double> true_topk_in_probe_v;
    std::vector<double> true_topk_rate_v;
    std::vector<double> num_s2_candidates_v;
    std::vector<double> dk_percentile_v;
    std::vector<double> count_s2_lt_dk_v;
    std::vector<double> count_true_topk_s2_lt_dk_v;
    std::vector<double> count_s2_safein_current_v;
    std::vector<double> max_true_topk_gap_v;
    std::vector<double> all_true_topk_gap_v;

    std::ofstream csv(outdir + "/dk_distance_summary.csv");
    csv << "query_id,num_candidates,num_s2_candidates,true_topk_in_probe,"
        << "true_topk_in_probe_rate,dk_percentile_s2,count_s2_lt_dk,"
        << "count_true_topk_s2_lt_dk,count_s2_safein_current,"
        << "count_true_topk_s2_safein_current,max_true_topk_gap_s2\n";
    csv << std::fixed << std::setprecision(6);

    for (const QueryStats* q : queries) {
        const double dk_percentile = DkPercentile(q->s2_dists, q->dk_static);
        const double true_topk_rate = static_cast<double>(q->true_topk_in_probe) /
                                      static_cast<double>(topk);
        const double max_true_gap = MaxOrZero(q->true_topk_gap_s2);

        g.num_candidates += q->num_candidates;
        g.num_s2_candidates += q->num_s2_candidates;
        g.true_topk_in_probe += q->true_topk_in_probe;
        g.count_s2_lt_dk += q->count_s2_lt_dk;
        g.count_true_topk_s2_lt_dk += q->count_true_topk_s2_lt_dk;
        g.count_s2_safein_current += q->count_s2_safein_current;
        g.count_true_topk_s2_safein_current += q->count_true_topk_s2_safein_current;
        if (q->true_topk_in_probe > 0) ++g.queries_with_any_true_topk;
        if (q->true_topk_in_probe >= 1) ++g.queries_with_ge_1_true_topk;
        if (q->true_topk_in_probe >= 2) ++g.queries_with_ge_2_true_topk;
        if (q->true_topk_in_probe > topk ||
            q->count_s2_safein_current > q->count_s2_lt_dk ||
            q->count_true_topk_s2_safein_current > q->count_true_topk_s2_lt_dk ||
            q->count_true_topk_s2_lt_dk > q->true_topk_in_probe) {
            ++g.invariant_violations;
        }

        true_topk_in_probe_v.push_back(static_cast<double>(q->true_topk_in_probe));
        true_topk_rate_v.push_back(true_topk_rate);
        num_s2_candidates_v.push_back(static_cast<double>(q->num_s2_candidates));
        dk_percentile_v.push_back(dk_percentile);
        count_s2_lt_dk_v.push_back(static_cast<double>(q->count_s2_lt_dk));
        count_true_topk_s2_lt_dk_v.push_back(static_cast<double>(q->count_true_topk_s2_lt_dk));
        count_s2_safein_current_v.push_back(static_cast<double>(q->count_s2_safein_current));
        max_true_topk_gap_v.push_back(max_true_gap);
        all_true_topk_gap_v.insert(all_true_topk_gap_v.end(),
                                   q->true_topk_gap_s2.begin(),
                                   q->true_topk_gap_s2.end());

        csv << q->query_id << ','
            << q->num_candidates << ','
            << q->num_s2_candidates << ','
            << q->true_topk_in_probe << ','
            << true_topk_rate << ','
            << dk_percentile << ','
            << q->count_s2_lt_dk << ','
            << q->count_true_topk_s2_lt_dk << ','
            << q->count_s2_safein_current << ','
            << q->count_true_topk_s2_safein_current << ','
            << max_true_gap << '\n';
    }

    std::ofstream json(outdir + "/dk_distance_summary.json");
    json << std::fixed << std::setprecision(6);
    json << "{\n";
    json << "  \"topk\": " << topk << ",\n";
    json << "  \"num_queries\": " << g.num_queries << ",\n";
    json << "  \"num_candidates\": " << g.num_candidates << ",\n";
    json << "  \"num_s2_candidates\": " << g.num_s2_candidates << ",\n";
    json << "  \"true_topk_in_probe\": " << g.true_topk_in_probe << ",\n";
    json << "  \"queries_with_any_true_topk\": " << g.queries_with_any_true_topk << ",\n";
    json << "  \"queries_with_ge_1_true_topk\": " << g.queries_with_ge_1_true_topk << ",\n";
    json << "  \"queries_with_ge_2_true_topk\": " << g.queries_with_ge_2_true_topk << ",\n";
    json << "  \"count_s2_lt_dk\": " << g.count_s2_lt_dk << ",\n";
    json << "  \"count_true_topk_s2_lt_dk\": " << g.count_true_topk_s2_lt_dk << ",\n";
    json << "  \"count_s2_safein_current\": " << g.count_s2_safein_current << ",\n";
    json << "  \"count_true_topk_s2_safein_current\": " << g.count_true_topk_s2_safein_current << ",\n";
    json << "  \"invariant_violations\": " << g.invariant_violations << ",\n";
    json << "  \"averages\": {\n";
    json << "    \"true_topk_in_probe\": " << Rate(g.true_topk_in_probe, g.num_queries) << ",\n";
    json << "    \"s2_lt_dk\": " << Rate(g.count_s2_lt_dk, g.num_queries) << ",\n";
    json << "    \"true_topk_s2_lt_dk\": " << Rate(g.count_true_topk_s2_lt_dk, g.num_queries) << ",\n";
    json << "    \"s2_safein_current\": " << Rate(g.count_s2_safein_current, g.num_queries) << ",\n";
    json << "    \"true_topk_s2_safein_current\": " << Rate(g.count_true_topk_s2_safein_current, g.num_queries) << ",\n";
    json << "    \"dk_percentile_s2\": " << Percentile(dk_percentile_v, 0.50) << "\n";
    json << "  },\n";
    json << "  \"per_query_percentiles\": {\n";
    WriteMetricJson(json, "true_topk_in_probe", true_topk_in_probe_v, true);
    WriteMetricJson(json, "true_topk_in_probe_rate", true_topk_rate_v, true);
    WriteMetricJson(json, "num_s2_candidates", num_s2_candidates_v, true);
    WriteMetricJson(json, "dk_percentile_s2", dk_percentile_v, true);
    WriteMetricJson(json, "count_s2_lt_dk", count_s2_lt_dk_v, true);
    WriteMetricJson(json, "count_true_topk_s2_lt_dk", count_true_topk_s2_lt_dk_v, true);
    WriteMetricJson(json, "count_s2_safein_current", count_s2_safein_current_v, true);
    WriteMetricJson(json, "max_true_topk_gap_s2", max_true_topk_gap_v, false);
    json << "  },\n";
    WriteMetricJson(json, "all_true_topk_gap_s2", all_true_topk_gap_v, false);
    json << "}\n";

    std::ofstream md(outdir + "/dk_distance_compare.md");
    md << "| metric | value |\n";
    md << "| --- | ---: |\n";
    md << "| avg true_topk_in_probe | " << Rate(g.true_topk_in_probe, g.num_queries) << " |\n";
    md << "| avg s2_est < d_k | " << Rate(g.count_s2_lt_dk, g.num_queries) << " |\n";
    md << "| avg true_topk && s2_est < d_k | " << Rate(g.count_true_topk_s2_lt_dk, g.num_queries) << " |\n";
    md << "| avg current SafeIn | " << Rate(g.count_s2_safein_current, g.num_queries) << " |\n";
    md << "| median dk_percentile_s2 | " << Percentile(dk_percentile_v, 0.50) << " |\n";
    md << "| queries with >=1 true topk | " << g.queries_with_ge_1_true_topk << " / " << g.num_queries << " |\n";
    md << "| invariant violations | " << g.invariant_violations << " |\n";

    Log("Loaded %lu queries and %lu candidates.\n",
        static_cast<unsigned long>(g.num_queries),
        static_cast<unsigned long>(g.num_candidates));
    Log("Wrote %s\n", (outdir + "/dk_distance_summary.csv").c_str());
    Log("Wrote %s\n", (outdir + "/dk_distance_summary.json").c_str());
    Log("Wrote %s\n", (outdir + "/dk_distance_compare.md").c_str());
    if (g.invariant_violations != 0) {
        std::fprintf(stderr, "Invariant violations: %lu\n",
                     static_cast<unsigned long>(g.invariant_violations));
        return 2;
    }
    return 0;
}
