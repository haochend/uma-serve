// UMA Serve - Tokenization helpers
#include "runtime/tokens.h"

#include "llama.h"

#include <string>
#include <vector>

namespace uma::runtime::tokens {

std::vector<int> tokenize(const llama_vocab* vocab, const std::string& text,
                          bool add_bos, bool special) {
    if (!vocab) return {};
    // two-pass: first pass with nullptr to get required length (negative)
    const int n = -llama_tokenize(vocab, text.c_str(), (int)text.size(), nullptr, 0, add_bos, special);
    if (n <= 0) return {};
    std::vector<int> out((size_t)n);
    const int rc = llama_tokenize(vocab, text.c_str(), (int)text.size(),
                                  reinterpret_cast<llama_token*>(out.data()), n, add_bos, special);
    if (rc < 0) return {};
    return out;
}

int token_to_piece(const llama_vocab* vocab, int token_id, char* buf, size_t buf_size,
                   bool special) {
    if (!vocab || !buf || buf_size == 0) return 0;
    return llama_token_to_piece(vocab, static_cast<llama_token>(token_id), buf, buf_size, 0, special);
}

std::string token_to_piece_str(const llama_vocab* vocab, int token_id, bool special) {
    char tmp[256];
    int n = token_to_piece(vocab, token_id, tmp, sizeof(tmp), special);
    if (n <= 0) return {};
    return std::string(tmp, tmp + n);
}

} // namespace uma::runtime::tokens

