#include "sched/scheduler.h"
#include "ipc/protocol.h"
#include "ipc/session.h"
#include "llama.h"
#include "runtime/tokens.h"

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
    // Experiment: start with full backend batch capacity to better utilize device during prefill
    target_batch_ = batch_cap_;
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
    std::vector<llama_pos> pos;
    pos.reserve(batch_cap_);

    struct SampleRef {
        int fd;
        int batch_index;
        uma::ipc::SessionState state_before;
    };
    std::vector<SampleRef> samples;
    samples.reserve(batch_cap_);

    std::vector<int> result_fds;

    // Use policy to plan this tick
    Plan plan = policy_.schedule_tick(sessions, batch_cap_, target_batch_, rr_decode_idx_,
                                      rr_prefill_idx_);
    // Apply RR cursor updates
    rr_decode_idx_ = plan.next_rr_decode_idx;
    rr_prefill_idx_ = plan.next_rr_prefill_idx;

    // Enact the plan: fill tokens arrays and session updates according to items
    for (const auto& item : plan.items) {
        auto it = sessions.find(item.fd);
        if (it == sessions.end()) continue;
        auto& s = *it->second;
        if (item.phase == uma::sched::Phase::DECODE) {
            llama_token t = static_cast<llama_token>(s.pending_tok);
            s.has_pending_tok = false;
            tokens.push_back(t);
            n_seq_id.push_back(1);
            seq_id_vals.push_back((llama_seq_id)s.seq);
            seq_ids.push_back(&seq_id_vals.back());
            pos.push_back((llama_pos)s.n_past);
            logits.push_back(1);
            samples.push_back({s.fd, (int)tokens.size() - 1, uma::ipc::SessionState::DECODE});
        } else { // PREFILL
            const int32_t chunk = item.n_tokens;
            assert(chunk >= 0 && "prefill chunk size is less than 0");
            const int32_t base_pos = s.n_past;
            for (int32_t j = 0; j < chunk; ++j) {
                llama_token t = static_cast<llama_token>(s.prompt_tokens[s.prefill_idx++]);
                tokens.push_back(t);
                n_seq_id.push_back(1);
                seq_id_vals.push_back(s.seq);
                seq_ids.push_back(&seq_id_vals.back());
                pos.push_back((llama_pos)(base_pos + j));
                int8_t lg = (j == chunk - 1) ? 1 : 0;
                logits.push_back(lg);
                if (lg) {
                    samples.push_back({s.fd, (int)tokens.size() - 1, uma::ipc::SessionState::PREFILL});
                }
            }
            s.n_past = base_pos + chunk;
        }
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
        batch.pos = pos.data();
        batch.n_seq_id = n_seq_id.data();
        batch.seq_id = seq_ids.data();
        batch.logits = logits.data();

        if (config_.enable_perf) {
            llama_perf_context_reset(ctx_);
        }
        auto t0 = std::chrono::steady_clock::now();
        int dec_rc = llama_decode(ctx_, batch);
        // Always synchronize to reflect real compute time in wall clock
        llama_synchronize(ctx_);
        auto t1 = std::chrono::steady_clock::now();
        auto dur_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        double ms = static_cast<double>(dur_ns) / 1.0e6;

        // update metrics (if provided)
        if (metrics_) {
            metrics_->batch_calls_total.fetch_add(1, std::memory_order_relaxed);
            metrics_->last_batch_size.store(static_cast<uint32_t>(tokens.size()),
                                            std::memory_order_relaxed);

            // Split accounting: attribute total time proportionally to token counts
            const uint64_t tot_tok = static_cast<uint64_t>(tokens.size());
            const uint64_t gen_tok = static_cast<uint64_t>(plan.decode_tok_count);
            const uint64_t pf_tok = static_cast<uint64_t>(plan.prefill_tok_count);
            metrics_->decode_phase_tokens_total.fetch_add(gen_tok, std::memory_order_relaxed);
            metrics_->prefill_tokens_total.fetch_add(pf_tok, std::memory_order_relaxed);

            uint64_t gen_ns = 0;
            uint64_t pf_ns = 0;
            if (tot_tok > 0) {
                gen_ns = static_cast<uint64_t>((__int128)dur_ns * gen_tok / tot_tok);
                pf_ns = static_cast<uint64_t>(dur_ns) - gen_ns;
            }
            metrics_->decode_ns_total_gen.fetch_add(gen_ns, std::memory_order_relaxed);
            metrics_->prefill_ns_total.fetch_add(pf_ns, std::memory_order_relaxed);

            // Generation-only decode metrics: exclude PREFILL
            if (gen_tok > 0) {
                uint32_t gen_ms_u32 = static_cast<uint32_t>((gen_ns / 1000000.0) + 0.5);
                metrics_->decode_ms_last.store(gen_ms_u32, std::memory_order_relaxed);
                metrics_->decode_ns_total.fetch_add(gen_ns, std::memory_order_relaxed);
                metrics_->decode_calls.fetch_add(1, std::memory_order_relaxed);
                metrics_->decode_tokens_total.fetch_add(gen_tok, std::memory_order_relaxed);
                // Min/max (single-threaded writer; relaxed is fine)
                uint32_t cur_min = metrics_->decode_ms_min.load(std::memory_order_relaxed);
                if (gen_ms_u32 < cur_min)
                    metrics_->decode_ms_min.store(gen_ms_u32, std::memory_order_relaxed);
                uint32_t cur_max = metrics_->decode_ms_max.load(std::memory_order_relaxed);
                if (gen_ms_u32 > cur_max)
                    metrics_->decode_ms_max.store(gen_ms_u32, std::memory_order_relaxed);
            }

            // llama internal perf breakdown (optional)
            if (config_.enable_perf) {
                auto pdata = llama_perf_context(ctx_);
                uint32_t eval_ms = static_cast<uint32_t>(pdata.t_eval_ms + 0.5);
                uint32_t p_eval_ms = static_cast<uint32_t>(pdata.t_p_eval_ms + 0.5);
                metrics_->eval_ms_last.store(eval_ms, std::memory_order_relaxed);
                metrics_->p_eval_ms_last.store(p_eval_ms, std::memory_order_relaxed);
                metrics_->eval_ns_total.fetch_add((uint64_t)(pdata.t_eval_ms * 1.0e6),
                                                  std::memory_order_relaxed);
                metrics_->p_eval_ns_total.fetch_add((uint64_t)(pdata.t_p_eval_ms * 1.0e6),
                                                    std::memory_order_relaxed);
                // increment calls if non-zero to avoid counting empty resets
                if (pdata.n_eval > 0)
                    metrics_->eval_calls.fetch_add(1, std::memory_order_relaxed);
                if (pdata.n_p_eval > 0)
                    metrics_->p_eval_calls.fetch_add(1, std::memory_order_relaxed);
            }
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
                        std::string piece =
                                uma::runtime::tokens::token_to_piece_str(vocab_, new_id, true);
                        if (!piece.empty()) {
                            uma::ipc::protocol::append_token_event(s.tx, s.request_id, piece,
                                                                   (int)new_id);
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
                        s.n_past = 0;
                        // Update last emit on EOS
                        if (s.first_emit_ns == 0)
                            s.first_emit_ns = now_ns;
                        s.last_emit_ns = now_ns;
                    } else {
                        {
                            std::string piece =
                                    uma::runtime::tokens::token_to_piece_str(vocab_, new_id, true);
                            if (!piece.empty()) {
                                uma::ipc::protocol::append_token_event(s.tx, s.request_id, piece,
                                                                       (int)new_id);
                            }
                        }
                        s.generated_count++;
                        s.pending_tok = new_id;
                        s.has_pending_tok = true;
                        s.n_past += 1; // we consumed the previously pending token this tick
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
