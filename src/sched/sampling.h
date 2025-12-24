// UMA Serve - Sampling interfaces (pluggable)
#pragma once

#include "runtime/tokens.h" // for llama_token typedef

#include <cstdint>
#include <random>
#include <vector>

namespace uma::sched {

struct SamplingParams {
    float temperature = 0.8f; // 0 => greedy
    float top_p = 0.95f;      // 1 => no nucleus
    int32_t top_k = 0;        // 0 => disabled
};

class ISampler {
  public:
    virtual ~ISampler() = default;
    virtual llama_token sample(const float* logits, int32_t n_vocab, const SamplingParams& params,
                               std::mt19937& rng) = 0;
};

// Default sampler: temperature + top-p (+ optional top-k)
class TopPSampler : public ISampler {
  public:
    llama_token sample(const float* logits, int32_t n_vocab, const SamplingParams& params,
                       std::mt19937& rng) override;
};

} // namespace uma::sched
