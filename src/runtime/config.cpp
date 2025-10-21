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
        return std::strcmp(s, "1") == 0 || strcasecmp(s, "true") == 0 || strcasecmp(s, "yes") == 0 || strcasecmp(s, "on") == 0;
    }
}

RuntimeConfig RuntimeConfig::from_args(int argc, char** argv) {
    RuntimeConfig cfg;

    // env defaults (YAML later)
    if (auto* m = get_env("UMA_MODEL"))   cfg.model_path = m;
    if (auto* s = get_env("UMA_N_CTX"))   cfg.n_ctx     = static_cast<uint32_t>(std::strtoul(s, nullptr, 10));
    if (auto* t = get_env("UMA_THREADS")) cfg.n_threads = static_cast<int32_t>(std::strtol(t, nullptr, 10));
    if (auto* sp = get_env("UMA_SOCK"))   cfg.socket_path = sp;
    if (auto* v = get_env("UMA_USE_MMAP"))  cfg.use_mmap  = parse_bool_flag(v);
    if (auto* v = get_env("UMA_USE_MLOCK")) cfg.use_mlock = parse_bool_flag(v);

    // simple argv parser
    for (int i = 1; i < argc; ++i) {
        std::string arg(argv[i]);
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) throw std::invalid_argument(std::string("missing value for ") + name);
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
        } else if (arg == "--kv-unified") {
            cfg.kv_unified = true;
        } else if (arg == "--no-kv-unified") {
            cfg.kv_unified = false;
        } else if (arg == "--no-offload-kqv") {
            cfg.offload_kqv = false;
        } else if (arg == "--offload-kqv") {
            cfg.offload_kqv = true;
        } else if (arg == "--no-swa-full") {
            cfg.swa_full = false;
        } else if (arg == "--swa-full") {
            cfg.swa_full = true;
        } else if (arg == "--sock" || arg == "--socket") {
            cfg.socket_path = need("--socket");
        } else if (arg == "--max-sessions") {
            cfg.max_sessions = static_cast<uint32_t>(std::strtoul(need("--max-sessions"), nullptr, 10));
        } else if (arg == "--max-prompt-bytes") {
            cfg.max_prompt_bytes = static_cast<uint32_t>(std::strtoul(need("--max-prompt-bytes"), nullptr, 10));
        } else if (arg == "--max-tokens") {
            cfg.max_tokens = static_cast<uint32_t>(std::strtoul(need("--max-tokens"), nullptr, 10));
        } else if (arg == "--idle-timeout-sec") {
            cfg.idle_timeout_sec = static_cast<uint32_t>(std::strtoul(need("--idle-timeout-sec"), nullptr, 10));
        } else if (arg == "--help" || arg == "-h") {
            throw std::invalid_argument("help");
        }
    }

    return cfg;
}

} // namespace uma::runtime
