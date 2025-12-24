// UMA Serve - RuntimeConfig (Week 1 minimal)
#include "config.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace uma::runtime {

namespace {
inline const char* get_env(const char* key) {
    const char* v = std::getenv(key);
    return v && *v ? v : nullptr;
}

inline bool parse_bool_flag(const char* s) {
    return std::strcmp(s, "1") == 0 || strcasecmp(s, "true") == 0 || strcasecmp(s, "yes") == 0 ||
           strcasecmp(s, "on") == 0;
}
} // namespace

RuntimeConfig RuntimeConfig::from_args(int argc, char** argv) {
    RuntimeConfig cfg;

    // env defaults (YAML later)
    if (auto* m = get_env("UMA_MODEL"))
        cfg.model_path = m;
    if (auto* s = get_env("UMA_N_CTX"))
        cfg.n_ctx = static_cast<uint32_t>(std::strtoul(s, nullptr, 10));
    if (auto* t = get_env("UMA_THREADS"))
        cfg.n_threads = static_cast<int32_t>(std::strtol(t, nullptr, 10));
    if (auto* b = get_env("UMA_N_BATCH"))
        cfg.n_batch = static_cast<uint32_t>(std::strtoul(b, nullptr, 10));
    if (auto* ub = get_env("UMA_N_UBATCH"))
        cfg.n_ubatch = static_cast<uint32_t>(std::strtoul(ub, nullptr, 10));
    if (auto* sp = get_env("UMA_SOCK"))
        cfg.socket_path = sp;
    if (auto* v = get_env("UMA_N_SEQ"))
        cfg.n_seq_max = static_cast<uint32_t>(std::strtoul(v, nullptr, 10));
    if (auto* v = get_env("UMA_USE_MMAP"))
        cfg.use_mmap = parse_bool_flag(v);
    if (auto* v = get_env("UMA_USE_MLOCK"))
        cfg.use_mlock = parse_bool_flag(v);
    if (auto* v = get_env("UMA_SLO_TTFT_MS"))
        cfg.slo_ttft_ms = (uint32_t)std::strtoul(v, nullptr, 10);
    if (auto* v = get_env("UMA_SLO_TBT_MS"))
        cfg.slo_tbt_ms = (uint32_t)std::strtoul(v, nullptr, 10);
    if (auto* v = get_env("UMA_BMT_BUDGET")) {
        // dimensionless token-attention units (uint64)
        cfg.bmt_budget_units = (uint64_t) std::strtoull(v, nullptr, 10);
    }

    // Gate debug features under UMA_LOG_LEVEL=debug
    {
        bool dbg = false;
        if (auto* l = get_env("UMA_LOG_LEVEL")) {
            if (*l) {
                std::string s(l);
                for (auto& c : s)
                    c = (char)std::tolower(c);
                dbg = (s == "debug");
            }
        }
        cfg.enable_perf = dbg;
    }

    // simple argv parser
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc)
                throw std::invalid_argument(std::string("missing value for ") + name);
            return argv[++i];
        };

        if (arg == "--model") {
            cfg.model_path = need("--model");
        } else if (arg == "--n-ctx") {
            cfg.n_ctx = static_cast<uint32_t>(std::strtoul(need("--n-ctx"), nullptr, 10));
        } else if (arg == "--threads") {
            cfg.n_threads = static_cast<int32_t>(std::strtol(need("--threads"), nullptr, 10));
        } else if (arg == "--mlock") {
            cfg.use_mlock = true;
        } else if (arg == "--no-mlock") {
            cfg.use_mlock = false;
        } else if (arg == "--no-mmap") {
            cfg.use_mmap = false;
        } else if (arg == "--mmap") {
            cfg.use_mmap = true;
        } else if (arg == "--sock" || arg == "--socket") {
            cfg.socket_path = need("--socket");
        } else if (arg == "--max-sessions") {
            cfg.max_sessions =
                    static_cast<uint32_t>(std::strtoul(need("--max-sessions"), nullptr, 10));
        } else if (arg == "--parallel" || arg == "--n-seq-max") {
            cfg.n_seq_max = static_cast<uint32_t>(std::strtoul(need("--parallel"), nullptr, 10));
        } else if (arg == "--max-tokens") {
            cfg.max_tokens = static_cast<uint32_t>(std::strtoul(need("--max-tokens"), nullptr, 10));
        } else if (arg == "--bmt-budget") {
            // experimental: dimensionless token-attention units
            cfg.bmt_budget_units = (uint64_t) std::strtoull(need("--bmt-budget"), nullptr, 10);
        } else if (arg == "--help" || arg == "-h") {
            throw std::invalid_argument("help");
        } else {
            // Strict mode: reject unknown flags to match common CLI behavior
            throw std::invalid_argument(std::string("unknown flag: ") + arg);
        }
    }

    return cfg;
}

} // namespace uma::runtime
