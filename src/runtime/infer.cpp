// UMA Serve - Minimal inference helpers (W1)
#include "runtime/infer.h"

#include "llama.h"

#include <string>
#include <vector>

namespace uma::runtime {

int generate_greedy_stream(
    llama_context* ctx,
    llama_model*   model,
    const std::string& prompt,
    int max_new_tokens,
    const std::function<void(const char*, size_t)>& on_piece) {

    if (max_new_tokens <= 0) max_new_tokens = 128;

    const llama_vocab* vocab = llama_model_get_vocab(model);

    // Clear prior state (KV, etc.)
    llama_memory_clear(llama_get_memory(ctx), false);

    // Tokenize prompt
    const int n_prompt = -llama_tokenize(vocab, prompt.c_str(), prompt.size(), nullptr, 0, true, true);
    if (n_prompt <= 0) {
        return 0;
    }
    std::vector<llama_token> prompt_tokens(n_prompt);
    if (llama_tokenize(vocab, prompt.c_str(), prompt.size(), prompt_tokens.data(), prompt_tokens.size(), true, true) < 0) {
        return 0;
    }

    // Show original prompt text (stream it back too for W1 simplicity)
    for (auto id : prompt_tokens) {
        char buf[256];
        int n = llama_token_to_piece(vocab, id, buf, sizeof(buf), 0, true);
        if (n > 0) on_piece(buf, static_cast<size_t>(n));
    }

    // Prepare batch for prompt
    llama_batch batch = llama_batch_get_one(prompt_tokens.data(), prompt_tokens.size());

    if (llama_model_has_encoder(model)) {
        if (llama_encode(ctx, batch)) {
            return 0;
        }
        llama_token decoder_start = llama_model_decoder_start_token(model);
        if (decoder_start == LLAMA_TOKEN_NULL) {
            decoder_start = llama_vocab_bos(vocab);
        }
        batch = llama_batch_get_one(&decoder_start, 1);
    }

    // Initialize a simple greedy sampler
    auto sparams = llama_sampler_chain_default_params();
    sparams.no_perf = true;
    llama_sampler* smpl = llama_sampler_chain_init(sparams);
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

    int n_decode = 0;
    for (int n_pos = 0; n_pos + batch.n_tokens < n_prompt + max_new_tokens; ) {
        if (llama_decode(ctx, batch)) {
            break;
        }
        n_pos += batch.n_tokens;

        // Greedy sampler
        llama_token new_id = llama_sampler_sample(smpl, ctx, -1);
        if (llama_vocab_is_eog(vocab, new_id)) {
            break;
        }

        char buf[256];
        int n = llama_token_to_piece(vocab, new_id, buf, sizeof(buf), 0, true);
        if (n > 0) on_piece(buf, static_cast<size_t>(n));

        batch = llama_batch_get_one(&new_id, 1);
        n_decode++;
    }

    // newline terminate stream for W1
    const char nl = '\n';
    on_piece(&nl, 1);

    llama_sampler_free(smpl);

    return n_decode;
}

} // namespace uma::runtime
