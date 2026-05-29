/// bench_safein_replay.cpp — Offline replay of SafeIn thresholds on top of an
/// existing per-candidate diagnostic CSV.
///
/// Usage:
///   bench_safein_replay --input /path/to/per_candidate.csv
///       [--outdir ./replay_output]
///       [--scales 0.50,0.55,0.60,...,0.99]

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
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

enum class ResultClass {
    SafeIn,
    SafeOut,
    Uncertain,
};

struct CandidateRow {
    bool is_true_topk = false;
    bool stage2_evaluated = false;
    double est_dist_s1 = 0.0;
    double est_dist_s2 = 0.0;
    double margin_s1 = 0.0;
    double margin_s2 = 0.0;
    double d_k_static = 0.0;
    double safeout_frontier_upper = 0.0;
};

struct Counts {
    uint64_t safein = 0;
    uint64_t safeout = 0;
    uint64_t uncertain = 0;
    uint64_t false_safein = 0;
    uint64_t false_safeout = 0;
};

struct ReplaySummary {
    double scale = 1.0;
    double eps_in_effective_s1 = 0.0;
    double eps_in_effective_s2 = 0.0;
    Counts s1;
    Counts s2;
    Counts final;
};

static std::string GetArg(int argc, char* argv[], const char* name,
                          const std::string& def) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    }
    return def;
}

static void Log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    va_end(ap);
    std::fflush(stdout);
}

static std::string Trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    size_t i = 0;
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    return s.substr(i);
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

static bool ParseBool01(const std::string& s) {
    return !s.empty() && s != "0";
}

static void AddCount(Counts* counts, ResultClass rc, bool is_true_topk) {
    if (rc == ResultClass::SafeIn) {
        ++counts->safein;
        if (!is_true_topk) ++counts->false_safein;
    } else if (rc == ResultClass::SafeOut) {
        ++counts->safeout;
        if (is_true_topk) ++counts->false_safeout;
    } else {
        ++counts->uncertain;
    }
}

static ResultClass ReplayClass(double est_dist,
                               double safeout_margin_current,
                               double safein_margin_replay,
                               double safeout_frontier_upper,
                               double d_k_static) {
    if (est_dist > safeout_frontier_upper + safeout_margin_current) {
        return ResultClass::SafeOut;
    }
    if (est_dist < d_k_static - safein_margin_replay) {
        return ResultClass::SafeIn;
    }
    return ResultClass::Uncertain;
}

static std::vector<double> ParseScales(const std::string& arg) {
    std::vector<double> out;
    std::stringstream ss(arg);
    std::string tok;
    while (std::getline(ss, tok, ',')) {
        tok = Trim(tok);
        if (!tok.empty()) out.push_back(ParseDoubleOr(tok, 0.0));
    }
    return out;
}

static double Rate(uint64_t num, uint64_t denom) {
    if (denom == 0) return 0.0;
    return static_cast<double>(num) / static_cast<double>(denom);
}

}  // namespace

int main(int argc, char* argv[]) {
    const std::string input = GetArg(argc, argv, "--input", "");
    const std::string outdir = GetArg(argc, argv, "--outdir", "./replay_output");
    const std::string scales_arg = GetArg(
        argc, argv, "--scales",
        "0.50,0.55,0.60,0.65,0.70,0.75,0.80,0.85,0.90,0.95,0.97,0.99");

    if (input.empty()) {
        std::fprintf(stderr,
                     "Usage: bench_safein_replay --input <per_candidate.csv> "
                     "[--outdir ./replay_output] [--scales 0.50,0.75,0.99]\n");
        return 1;
    }

    const std::vector<double> scales = ParseScales(scales_arg);
    if (scales.empty()) {
        std::fprintf(stderr, "No valid scales parsed.\n");
        return 1;
    }

    fs::create_directories(outdir);
    Log("=== SafeIn Replay ===\n");
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
        "is_true_topk", "est_dist_s1", "est_dist_s2", "margin_s1",
        "margin_s2_current", "d_k_static", "dynamic_d_k_before_cluster",
        "s2_class"
    };
    for (const char* key : required) {
        if (!col.count(key)) {
            std::fprintf(stderr, "Missing required column: %s\n", key);
            return 1;
        }
    }

    std::vector<CandidateRow> rows;
    rows.reserve(1 << 20);
    std::string line;
    while (std::getline(f, line)) {
        std::vector<std::string> fields = SplitCsvLine(line);
        if (fields.size() < header.size()) fields.resize(header.size());
        CandidateRow row;
        row.is_true_topk = ParseBool01(fields[col["is_true_topk"]]);
        row.est_dist_s1 = ParseDoubleOr(fields[col["est_dist_s1"]], 0.0);
        row.margin_s1 = ParseDoubleOr(fields[col["margin_s1"]], 0.0);
        row.d_k_static = ParseDoubleOr(fields[col["d_k_static"]], 0.0);
        row.safeout_frontier_upper =
            ParseDoubleOr(fields[col["dynamic_d_k_before_cluster"]],
                          std::numeric_limits<double>::infinity());
        row.stage2_evaluated = !fields[col["est_dist_s2"]].empty();
        if (row.stage2_evaluated) {
            row.est_dist_s2 = ParseDoubleOr(fields[col["est_dist_s2"]], 0.0);
            row.margin_s2 = ParseDoubleOr(fields[col["margin_s2_current"]], 0.0);
        }
        rows.push_back(row);
    }
    if (rows.empty()) {
        std::fprintf(stderr, "No data rows loaded from %s\n", input.c_str());
        return 1;
    }

    const double baseline_margin_s1 = rows.front().margin_s1;
    const double baseline_margin_s2 = rows.front().margin_s2;
    std::vector<ReplaySummary> summaries;
    summaries.reserve(scales.size());

    for (double scale : scales) {
        ReplaySummary summary;
        summary.scale = scale;
        summary.eps_in_effective_s1 = scale;
        summary.eps_in_effective_s2 = scale;
        for (const CandidateRow& row : rows) {
            const double replay_margin_s1 = row.margin_s1 * scale;
            const ResultClass s1 = ReplayClass(
                row.est_dist_s1, row.margin_s1, replay_margin_s1,
                row.safeout_frontier_upper, row.d_k_static);
            AddCount(&summary.s1, s1, row.is_true_topk);

            ResultClass final_rc = s1;
            if (row.stage2_evaluated) {
                const double replay_margin_s2 = row.margin_s2 * scale;
                const ResultClass s2 = ReplayClass(
                    row.est_dist_s2, row.margin_s2, replay_margin_s2,
                    row.safeout_frontier_upper, row.d_k_static);
                AddCount(&summary.s2, s2, row.is_true_topk);
                final_rc = s2;
            }
            AddCount(&summary.final, final_rc, row.is_true_topk);
        }
        summaries.push_back(summary);
    }

    std::ofstream csv(outdir + "/replay_summary.csv");
    csv << "eps_in_scale,eps_in_effective_s1,eps_in_effective_s2,"
        << "s1_safein,s1_safeout,s1_uncertain,s1_false_safein,s1_false_safeout,s1_false_safein_rate,s1_false_safeout_rate,"
        << "s2_safein,s2_safeout,s2_uncertain,s2_false_safein,s2_false_safeout,s2_false_safein_rate,s2_false_safeout_rate,"
        << "final_safein,final_safeout,final_uncertain,final_false_safein,final_false_safeout,final_false_safein_rate,final_false_safeout_rate\n";
    csv << std::fixed << std::setprecision(6);
    for (const auto& s : summaries) {
        csv << s.scale << ',' << s.eps_in_effective_s1 << ',' << s.eps_in_effective_s2 << ','
            << s.s1.safein << ',' << s.s1.safeout << ',' << s.s1.uncertain << ','
            << s.s1.false_safein << ',' << s.s1.false_safeout << ','
            << Rate(s.s1.false_safein, s.s1.safein) << ','
            << Rate(s.s1.false_safeout, s.s1.safeout) << ','
            << s.s2.safein << ',' << s.s2.safeout << ',' << s.s2.uncertain << ','
            << s.s2.false_safein << ',' << s.s2.false_safeout << ','
            << Rate(s.s2.false_safein, s.s2.safein) << ','
            << Rate(s.s2.false_safeout, s.s2.safeout) << ','
            << s.final.safein << ',' << s.final.safeout << ',' << s.final.uncertain << ','
            << s.final.false_safein << ',' << s.final.false_safeout << ','
            << Rate(s.final.false_safein, s.final.safein) << ','
            << Rate(s.final.false_safeout, s.final.safeout) << '\n';
    }

    std::ofstream json(outdir + "/replay_summary.json");
    json << "[\n";
    for (size_t i = 0; i < summaries.size(); ++i) {
        const auto& s = summaries[i];
        json << "  {\n";
        json << "    \"eps_in_scale\": " << std::fixed << std::setprecision(6) << s.scale << ",\n";
        json << "    \"eps_in_effective_s1\": " << s.eps_in_effective_s1 << ",\n";
        json << "    \"eps_in_effective_s2\": " << s.eps_in_effective_s2 << ",\n";
        auto write_counts = [&](const char* name, const Counts& c, bool comma) {
            json << "    \"" << name << "\": {\n";
            json << "      \"safein\": " << c.safein << ",\n";
            json << "      \"safeout\": " << c.safeout << ",\n";
            json << "      \"uncertain\": " << c.uncertain << ",\n";
            json << "      \"false_safein\": " << c.false_safein << ",\n";
            json << "      \"false_safeout\": " << c.false_safeout << ",\n";
            json << "      \"false_safein_rate\": " << Rate(c.false_safein, c.safein) << ",\n";
            json << "      \"false_safeout_rate\": " << Rate(c.false_safeout, c.safeout) << "\n";
            json << "    }" << (comma ? "," : "") << "\n";
        };
        write_counts("s1", s.s1, true);
        write_counts("s2", s.s2, true);
        write_counts("final", s.final, false);
        json << "  }" << (i + 1 == summaries.size() ? "\n" : ",\n");
    }
    json << "]\n";

    std::ofstream md(outdir + "/replay_compare.md");
    md << "| scale | final_safein | final_false_safein | final_false_safein_rate | final_safeout | final_false_safeout |\n";
    md << "| --- | ---: | ---: | ---: | ---: | ---: |\n";
    for (const auto& s : summaries) {
        md << "| " << std::fixed << std::setprecision(2) << s.scale
           << " | " << s.final.safein
           << " | " << s.final.false_safein
           << " | " << std::setprecision(6) << Rate(s.final.false_safein, s.final.safein)
           << " | " << s.final.safeout
           << " | " << s.final.false_safeout
           << " |\n";
    }

    Log("Loaded %zu candidate rows.\n", rows.size());
    Log("Baseline margin sample: s1=%.6f s2=%.6f\n", baseline_margin_s1, baseline_margin_s2);
    Log("Wrote %s\n", (outdir + "/replay_summary.csv").c_str());
    Log("Wrote %s\n", (outdir + "/replay_summary.json").c_str());
    Log("Wrote %s\n", (outdir + "/replay_compare.md").c_str());
    return 0;
}
