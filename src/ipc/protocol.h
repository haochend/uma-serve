// UMA Serve - Framed JSON protocol helpers (UDS)
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace uma::ipc::protocol {

// Maximum allowed JSON frame payload size (bytes). Config may override.
constexpr size_t kDefaultMaxFrameBytes = 1 * 1024 * 1024; // 1 MiB

// Attempt to parse one complete frame from rx buffer.
// Returns true and assigns out_json when a full frame is available and removed from rx.
// Returns false if more bytes are needed.
// If an oversize frame is detected, sets err_msg and returns false; caller should close.
bool try_read_frame(std::vector<uint8_t>& rx, std::string& out_json, size_t max_frame_bytes,
                    std::string* err_msg);

// Append a length-prefixed JSON frame to tx buffer.
void write_frame(std::vector<uint8_t>& tx, const std::string& json);

// Minimal JSON escape for strings (UTF-8 safe; escapes quotes, backslash, control chars)
std::string json_escape(const std::string& s);

// Helpers to build common event frames and append to tx
void append_token_event(std::vector<uint8_t>& tx, const std::string& id, const std::string& text,
                        int token_id);
void append_eos_event(std::vector<uint8_t>& tx, const std::string& id, const std::string& reason);
void append_error_event(std::vector<uint8_t>& tx, const std::string& id, const std::string& code,
                        const std::string& message);

} // namespace uma::ipc::protocol

