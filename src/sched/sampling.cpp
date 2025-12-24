// UMA Serve - Sampling implementations
#include "sched/sampling.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace uma::sched {

static inline float stable_max(const float* x, int32_t n) {
    float m = x[0];
    for (int32_t i = 1; i < n; ++i) m = std::max(m, x[i]);
    return m;
}

llama_token TopPSampler::sample(const float* logits, int32_t n_vocab, const SamplingParams& p,
                                std::mt19937& rng) {
    if (n_vocab <= 0) return 0;

    // Greedy if temperature <= 0
    if (p.temperature <= 0.0f) {
        int best_id = 0;
        float bestv = logits[0];
        for (int i = 1; i < n_vocab; ++i) {
            if (logits[i] > bestv) { bestv = logits[i]; best_id = i; }
        }
        return (llama_token) best_id;
    }

    // Build indices sorted by logit desc (optionally limited by top_k)
    std::vector<int> idx(n_vocab);
    for (int i = 0; i < n_vocab; ++i) idx[i] = i;
    std::partial_sort(idx.begin(),
                      p.top_k > 0 && p.top_k < n_vocab ? idx.begin() + p.top_k : idx.end(),
                      idx.end(),
                      [&](int a, int b) { return logits[a] > logits[b]; });
    int use_n = n_vocab;
    if (p.top_k > 0 && p.top_k < n_vocab) use_n = p.top_k;

    // Temperature scaling + softmax on truncated set
    const float inv_t = 1.0f / p.temperature;
    std::vector<float> probs((size_t)use_n);
    float max_logit = -std::numeric_limits<float>::infinity();
    for (int i = 0; i < use_n; ++i) max_logit = std::max(max_logit, logits[idx[i]] * inv_t);
    float sum = 0.0f;
    for (int i = 0; i < use_n; ++i) {
        float v = std::exp((logits[idx[i]] * inv_t) - max_logit);
        probs[(size_t)i] = v;
        sum += v;
    }
    if (sum <= 0.0f || !std::isfinite(sum)) {
        // fallback to greedy
        int best_id = idx[0];
        return (llama_token) best_id;
    }
    for (int i = 0; i < use_n; ++i) probs[(size_t)i] /= sum;

    // Apply top-p: keep smallest prefix with cumulative >= top_p
    float top_p = std::min(std::max(p.top_p, 0.0f), 1.0f);
    int cut = use_n;
    if (top_p < 0.9999f) {
        float c = 0.0f;
        cut = 0;
        for (; cut < use_n; ++cut) {
            c += probs[(size_t)cut];
            if (c >= top_p) { ++cut; break; }
        }
        if (cut <= 0) cut = 1;
    }

    // Renormalize over the kept prefix (important if cut < use_n)
    float csum = 0.0f;
    for (int i = 0; i < cut; ++i) csum += probs[(size_t)i];
    for (int i = 0; i < cut; ++i) probs[(size_t)i] /= csum;

    // Draw
    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    float r = uni(rng);
    float acc = 0.0f;
    for (int i = 0; i < cut; ++i) {
        acc += probs[(size_t)i];
        if (r <= acc || i == cut - 1) return (llama_token) idx[i];
    }
    return (llama_token) idx[0];
}

} // namespace uma::sched

