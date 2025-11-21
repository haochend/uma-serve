#include "sched/scheduler.h"
#include "ipc/session.h"
#include "llama.h"
#include "runtime/tokens.h"
#include "ipc/protocol.h"

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace uma::sched {

Scheduler::Scheduler(llama_context* ctx, const llama_vocab* vocab,
                     const runtime::RuntimeConfig& cfg, uma::metrics::Metrics* m)
    : ctx_(ctx), vocab_(vocab), config_(cfg), metrics_(m) {
    batch_cap_ = llama_n_batch(ctx);
    target_batch_ = std::min<int32_t>(batch_cap_, 32);
    rr_decode_idx_ = rr_prefill_idx_ = 0;
    decode_ms_ewma_ = tick_budget_ms_;
    if (metrics_) {
        metrics_->set_decode_ms_ewma(decode_ms_ewma_);
    }
}

std::vector<int> Scheduler::tick(ipc::SessionPool& sessions, uint64_t now_ns) {
    std::vector<llama_token> tokens;
    tokens.reserve(batch_cap_);
    std::vector<int32_t> n_seq_id;
    n_seq_id.reserve(batch_cap_);
    std::vector<llama_seq_id> seq_id_vals;
    seq_id_vals.reserve(batch_cap_);
    std::vector<llama_seq_id*> seq_ids;
    seq_ids.reserve(batch_cap_);
    std::vector<int8_t> logits;
    logits.reserve(batch_cap_);

    struct SampleRef {
        int fd;
        int batch_index;
        uma::ipc::SessionState state_before;
    };
    std::vector<SampleRef> samples;
    samples.reserve(batch_cap_);

    std::vector<int> result_fds;

    std::vector<int> decode_pool;
    std::vector<int> prefill_pool;
    for (auto& kv : sessions) {
        auto& s = *kv.second;
        if (s.state == ipc::SessionState::DECODE && s.has_pending_tok) {
            decode_pool.push_back(s.fd);
        } else if (s.state == ipc::SessionState::PREFILL &&
                   s.prefill_idx < s.prompt_tokens.size()) {
            prefill_pool.push_back(s.fd);
        }
    }

    int32_t budget = std::min<int32_t>(target_batch_, batch_cap_);
    assert(budget > 0 && "batch budget must be > 0");

    // handling Decode: give exactly 1 token to each ready DECODE session (round-robin), up to
    // budget
    if (!decode_pool.empty() && budget > 0) {
        for (size_t i = 0; i < decode_pool.size() && budget > 0; ++i) {
            int curr_fd = decode_pool[(rr_decode_idx_ + i) % decode_pool.size()];
            auto it = sessions.find(curr_fd);
            if (it == sessions.end()) {
                continue;
            }
            auto& s = *it->second;
            // Append the pending token
            llama_token t = static_cast<llama_token>(s.pending_tok);
            s.has_pending_tok = false;
            tokens.push_back(t);
            n_seq_id.push_back(1);
            seq_id_vals.push_back((llama_seq_id)s.seq);
            seq_ids.push_back(&seq_id_vals.back());
            logits.push_back(1);
            samples.push_back({s.fd, (int)tokens.size() - 1, uma::ipc::SessionState::DECODE});
            budget--;
        }
        rr_decode_idx_ = (rr_decode_idx_ + 1) % decode_pool.size();
    }

    // handling Prefill: drain large chunks up to remaining budget.
    // Prioritize sessions that haven't emitted their first token (TTFT-first), then others.
    if (!prefill_pool.empty() && budget > 0) {
        std::vector<int> ttft_pool;
        std::vector<int> rest_pool;
        ttft_pool.reserve(prefill_pool.size());
        rest_pool.reserve(prefill_pool.size());
        for (size_t i = 0; i < prefill_pool.size(); ++i) {
            int curr_fd = prefill_pool[(rr_prefill_idx_ + i) % prefill_pool.size()];
            auto it = sessions.find(curr_fd);
            if (it == sessions.end()) continue;
            auto& s = *it->second;
            if (s.first_emit_ns == 0) ttft_pool.push_back(curr_fd); else rest_pool.push_back(curr_fd);
        }
        auto schedule_pool = [&](const std::vector<int>& pool) {
            for (int curr_fd : pool) {
                if (budget <= 0) break;
                auto it = sessions.find(curr_fd);
                if (it == sessions.end()) continue;
                auto& s = *it->second;
                size_t remain_sz = s.prompt_tokens.size() - s.prefill_idx;
                int32_t remain = static_cast<int32_t>(std::min(remain_sz, static_cast<size_t>(INT32_MAX)));
                int32_t chunk = std::min<int32_t>(remain, budget);
                // Limit per-session burst for TTFT sessions to avoid starving others
                if (s.first_emit_ns == 0) {
                    const int32_t kBurst = 16;
                    chunk = std::min<int32_t>(chunk, kBurst);
                }
                assert(chunk >= 0 && "prefill chunk size is less than 0");
                for (int32_t j = 0; j < chunk; ++j) {
                    llama_token t = static_cast<llama_token>(s.prompt_tokens[s.prefill_idx++]);
                    tokens.push_back(t);
                    n_seq_id.push_back(1);
                    seq_id_vals.push_back(s.seq);
                    seq_ids.push_back(&seq_id_vals.back());
                    int8_t lg = (j == chunk - 1) ? 1 : 0;
                    logits.push_back(lg);
                    if (lg) {
                        samples.push_back({s.fd, (int)tokens.size() - 1, uma::ipc::SessionState::PREFILL});
                    }
                }
                budget -= chunk;
            }
        };
        schedule_pool(ttft_pool);
        if (budget > 0) schedule_pool(rest_pool);
        rr_prefill_idx_ = (rr_prefill_idx_ + 1) % prefill_pool.size();
    }

    if (!tokens.empty()) {
        // batch arrays should be in lockstep
        assert(n_seq_id.size() == tokens.size());
        assert(seq_id_vals.size() == tokens.size());
        assert(seq_ids.size() == tokens.size());
        assert(logits.size() == tokens.size());
        // ensure we don't exceed API limits
        assert(tokens.size() <= static_cast<size_t>(batch_cap_) && "batch exceeds llama_n_batch");
        assert(tokens.size() <= static_cast<size_t>(INT32_MAX) && "n_tokens must fit int32");
        // logits rows must match samples count
        size_t ones = static_cast<size_t>(std::count(logits.begin(), logits.end(), 1));
        assert(ones == samples.size() && "logits==1 count must equal samples");
        llama_batch batch{};
        batch.n_tokens = static_cast<int32_t>(tokens.size());
        batch.token = tokens.data();
        batch.embd = nullptr;
        batch.pos = nullptr; // auto-track positions per seq
        batch.n_seq_id = n_seq_id.data();
        batch.seq_id = seq_ids.data();
        batch.logits = logits.data();

        auto t0 = std::chrono::steady_clock::now();
        int dec_rc = llama_decode(ctx_, batch);
        // Ensure all queued backend work has finished so timing reflects real compute
        llama_synchronize(ctx_);
        auto t1 = std::chrono::steady_clock::now();
        double ms = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count() / 1000.0;

        // update metrics (if provided)
        if (metrics_) {
            metrics_->batch_calls_total.fetch_add(1, std::memory_order_relaxed);
            metrics_->last_batch_size.store(static_cast<uint32_t>(tokens.size()),
                                            std::memory_order_relaxed);
            metrics_->decode_ms_last.store(static_cast<uint32_t>(ms + 0.5),
                                           std::memory_order_relaxed);
        }
        // EWMA toward observed decode time + publish
        decode_ms_ewma_ = 0.8 * decode_ms_ewma_ + 0.2 * ms;
        if (metrics_)
            metrics_->set_decode_ms_ewma(decode_ms_ewma_);
        // Simple adaptive tuning
        if (decode_ms_ewma_ > 1.3 * tick_budget_ms_) {
            target_batch_ = std::max<int32_t>(8, (int32_t)(target_batch_ * 0.7));
        } else if (decode_ms_ewma_ < 0.8 * tick_budget_ms_) {
            target_batch_ = std::min<int32_t>(
                    batch_cap_, target_batch_ + std::max<int32_t>(1, target_batch_ / 8));
        }

        if (dec_rc != 0) {
            for (auto& sample : samples) {
                auto it = sessions.find(sample.fd);
                if (it == sessions.end()) {
                    continue;
                }
                auto& s = *it->second;
                s.last_error = "decode error";
                s.state = ipc::SessionState::ERRORED;
                uma::ipc::protocol::append_error_event(s.tx, s.request_id, "E_RUNTIME_DECODE",
                                                      "decode failed");
                s.read_closed = true;
            }
        } else {
            const int32_t n_vocab = llama_vocab_n_tokens(vocab_);
            for (size_t i = 0; i < samples.size(); ++i) {
                auto& sample = samples[i];
                auto it = sessions.find(sample.fd);
                if (it == sessions.end()) {
                    continue;
                }
                auto& s = *it->second;
                bool need_arm = s.tx.empty();
                float* logits_row = llama_get_logits_ith(ctx_, sample.batch_index);
                if (logits_row == nullptr) {
                    continue;
                }
                // greedy sampling
                int best_id = 0;
                float bestv = logits_row[0];
                for (size_t j = 1; j < n_vocab; ++j) {
                    if (logits_row[j] > bestv) {
                        best_id = j;
                        bestv = logits_row[j];
                    }
                }
                llama_token new_id = static_cast<llama_token>(best_id);
                if (sample.state_before == ipc::SessionState::PREFILL) {
                    // transition to DECODE; feed this token next tick
                    s.pending_tok = new_id;
                    s.has_pending_tok = true;
                    s.state = ipc::SessionState::DECODE;
                    {
                        std::string piece = uma::runtime::tokens::token_to_piece_str(vocab_, new_id, true);
                        if (!piece.empty()) {
                            uma::ipc::protocol::append_token_event(s.tx, s.request_id, piece, (int)new_id);
                        }
                    }
                    if (s.first_emit_ns == 0)
                        s.first_emit_ns = now_ns;
                    s.last_emit_ns = now_ns;
                    if (metrics_)
                        metrics_->tokens_generated_total.fetch_add(1, std::memory_order_relaxed);
                } else {
                    if (llama_vocab_is_eog(vocab_, new_id) ||
                        s.generated_count >= config_.max_tokens) {
                        uma::ipc::protocol::append_eos_event(
                                s.tx, s.request_id,
                                s.generated_count >= config_.max_tokens ? "length" : "stop");
                        s.state = ipc::SessionState::STREAM;
                        llama_memory_seq_rm(llama_get_memory(ctx_), s.seq, -1, -1);
                        // Update last emit on EOS
                        if (s.first_emit_ns == 0)
                            s.first_emit_ns = now_ns;
                        s.last_emit_ns = now_ns;
                    } else {
                        {
                            std::string piece = uma::runtime::tokens::token_to_piece_str(vocab_, new_id, true);
                            if (!piece.empty()) {
                                uma::ipc::protocol::append_token_event(s.tx, s.request_id, piece, (int)new_id);
                            }
                        }
                        s.generated_count++;
                        s.pending_tok = new_id;
                        s.has_pending_tok = true;
                        s.state = ipc::SessionState::DECODE;
                        if (s.first_emit_ns == 0)
                            s.first_emit_ns = now_ns;
                        s.last_emit_ns = now_ns;
                        if (metrics_)
                            metrics_->tokens_generated_total.fetch_add(1,
                                                                       std::memory_order_relaxed);
                    }
                }
                if (need_arm) {
                    result_fds.push_back(s.fd);
                }
            }
        }
    }

    return result_fds;
}

} // namespace uma::sched
