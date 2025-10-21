// UMA Serve - Minimal inference helpers (W1)
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct llama_context;
struct llama_model;

namespace uma::runtime {

// Greedy generate and stream piece-by-piece via callback (raw bytes)
// Returns number of new tokens generated.
int generate_greedy_stream(
    llama_context* ctx,
    llama_model*   model,
    const std::string& prompt,
    int max_new_tokens,
    const std::function<void(const char*, size_t)>& on_piece);

} // namespace uma::runtime
