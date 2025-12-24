#include "gtest/gtest.h"

#include "sched/sampling.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

using uma::sched::SamplingParams;
using uma::sched::TopPSampler;

namespace {

// Helper: compute expected sample following the same algorithm as TopPSampler
static int sample_expected(const std::vector<float>& logits, const SamplingParams& p,
                           std::mt19937& rng) {
    const int n_vocab = (int) logits.size();
    if (n_vocab <= 0) return 0;
    if (p.temperature <= 0.0f) {
        int best = 0; float bestv = logits[0];
        for (int i = 1; i < n_vocab; ++i) if (logits[i] > bestv) { best = i; bestv = logits[i]; }
        return best;
    }
    std::vector<int> idx(n_vocab);
    for (int i = 0; i < n_vocab; ++i) idx[i] = i;
    std::sort(idx.begin(), idx.end(), [&](int a, int b) { return logits[a] > logits[b]; });
    int use_n = n_vocab;
    if (p.top_k > 0 && p.top_k < n_vocab) use_n = p.top_k;

    const float inv_t = 1.0f / p.temperature;
    std::vector<float> probs((size_t)use_n);
    float max_logit = logits[idx[0]] * inv_t;
    for (int i = 1; i < use_n; ++i) max_logit = std::max(max_logit, logits[idx[i]] * inv_t);
    float sum = 0.0f;
    for (int i = 0; i < use_n; ++i) {
        float v = std::exp((logits[idx[i]] * inv_t) - max_logit);
        probs[(size_t)i] = v;
        sum += v;
    }
    if (sum <= 0.0f || !std::isfinite(sum)) {
        return idx[0];
    }
    for (int i = 0; i < use_n; ++i) probs[(size_t)i] /= sum;
    float tp = std::min(std::max(p.top_p, 0.0f), 1.0f);
    int cut = use_n;
    if (tp < 0.9999f) {
        float c = 0.0f;
        cut = 0;
        for (; cut < use_n; ++cut) {
            c += probs[(size_t)cut];
            if (c >= tp) { ++cut; break; }
        }
        if (cut <= 0) cut = 1;
    }
    float csum = 0.0f;
    for (int i = 0; i < cut; ++i) csum += probs[(size_t)i];
    for (int i = 0; i < cut; ++i) probs[(size_t)i] /= csum;

    std::uniform_real_distribution<float> uni(0.0f, 1.0f);
    float r = uni(rng);
    float acc = 0.0f;
    for (int i = 0; i < cut; ++i) {
        acc += probs[(size_t)i];
        if (r <= acc || i == cut - 1) return idx[i];
    }
    return idx[0];
}

} // namespace

TEST(SamplingTest, GreedyPicksArgmaxWhenTemperatureZero) {
    TopPSampler sampler;
    SamplingParams sp; sp.temperature = 0.0f; sp.top_p = 1.0f; sp.top_k = 0;
    std::vector<float> logits = { 0.1f, 2.0f, 0.5f, -1.0f };
    std::mt19937 rng(123);
    llama_token tok = sampler.sample(logits.data(), (int)logits.size(), sp, rng);
    EXPECT_EQ(tok, 1);
}

TEST(SamplingTest, NonZeroTempWithSmallTopPBehavesLikeGreedy) {
    TopPSampler sampler;
    SamplingParams sp; sp.temperature = 0.8f; sp.top_p = 0.5f; sp.top_k = 0;
    std::vector<float> logits = { 2.0f, 1.0f, 0.0f, -1.0f };
    std::mt19937 rng(12345);
    llama_token tok = sampler.sample(logits.data(), (int)logits.size(), sp, rng);
    // With top_p small enough, only top-1 remains after nucleus cutoff
    EXPECT_EQ(tok, 0);
}

TEST(SamplingTest, TopKOneIsDeterministicTop) {
    TopPSampler sampler;
    SamplingParams sp; sp.temperature = 0.7f; sp.top_p = 1.0f; sp.top_k = 1;
    std::vector<float> logits = { 0.0f, 10.0f, 9.0f, -5.0f, 8.0f };
    std::mt19937 rng(7);
    llama_token tok = sampler.sample(logits.data(), (int)logits.size(), sp, rng);
    EXPECT_EQ(tok, 1);
}

TEST(SamplingTest, TopPWithRngMatchesGoldenForSeed) {
    TopPSampler sampler;
    SamplingParams sp; sp.temperature = 1.0f; sp.top_p = 0.9f; sp.top_k = 0;
    std::vector<float> logits = { 2.0f, 1.0f, 0.0f, -1.0f };
    std::mt19937 rng_expected(42);
    std::mt19937 rng_for_sampler(42);
    int expected = sample_expected(logits, sp, rng_expected);
    llama_token tok = sampler.sample(logits.data(), (int)logits.size(), sp, rng_for_sampler);
    EXPECT_EQ(tok, expected);
}

TEST(SamplingTest, TopKRestrictsDomainAndMatchesGolden) {
    TopPSampler sampler;
    SamplingParams sp; sp.temperature = 0.7f; sp.top_p = 1.0f; sp.top_k = 2;
    std::vector<float> logits = { 0.0f, 10.0f, 9.0f, -5.0f, 8.0f };
    std::mt19937 rng_expected(1234);
    std::mt19937 rng_for_sampler(1234);
    int expected = sample_expected(logits, sp, rng_expected);
    llama_token tok = sampler.sample(logits.data(), (int)logits.size(), sp, rng_for_sampler);
    // domain should be indices {1,2}; golden ensures exact pick for this seed
    EXPECT_TRUE(tok == 1 || tok == 2);
    EXPECT_EQ(tok, expected);
}

