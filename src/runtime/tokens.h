// UMA Serve - Tokenization helpers
#pragma once

#include <string>
#include <vector>

struct llama_vocab;
typedef int llama_token;

namespace uma::runtime::tokens {

// Tokenize a string using llama.cpp vocab. Returns token ids (empty on failure).
std::vector<int> tokenize(const llama_vocab* vocab, const std::string& text,
                          bool add_bos, bool special);

// Convert token id to UTF-8 piece; returns number of bytes written (0 on none).
int token_to_piece(const llama_vocab* vocab, int token_id, char* buf, size_t buf_size,
                   bool special = true);

// Convenience: return piece as std::string (empty if none)
std::string token_to_piece_str(const llama_vocab* vocab, int token_id, bool special = true);

} // namespace uma::runtime::tokens

