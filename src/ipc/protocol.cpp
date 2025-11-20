// UMA Serve - Framed JSON protocol helpers (UDS)
#include "ipc/protocol.h"

#include <cstring>

namespace uma::ipc::protocol {

static inline uint32_t read_u32_le(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool try_read_frame(std::vector<uint8_t>& rx, std::string& out_json, size_t max_frame_bytes,
                    std::string* err_msg) {
    if (rx.size() < 4) return false;
    uint32_t len = read_u32_le(rx.data());
    if (len == 0) {
        if (err_msg) *err_msg = "invalid frame length 0";
        return false;
    }
    if (len > max_frame_bytes) {
        if (err_msg) *err_msg = "frame too large";
        return false;
    }
    if (rx.size() < 4u + (size_t)len) return false;
    out_json.assign(reinterpret_cast<const char*>(rx.data() + 4), len);
    rx.erase(rx.begin(), rx.begin() + 4 + len);
    return true;
}

void write_frame(std::vector<uint8_t>& tx, const std::string& json) {
    uint32_t len = (uint32_t)json.size();
    uint8_t hdr[4] = { (uint8_t)(len & 0xFF), (uint8_t)((len >> 8) & 0xFF),
                       (uint8_t)((len >> 16) & 0xFF), (uint8_t)((len >> 24) & 0xFF) };
    tx.insert(tx.end(), hdr, hdr + 4);
    tx.insert(tx.end(), json.begin(), json.end());
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned)c);
                    out += buf;
                } else {
                    out.push_back((char)c);
                }
        }
    }
    return out;
}

void append_token_event(std::vector<uint8_t>& tx, const std::string& id, const std::string& text,
                        int token_id) {
    std::string payload;
    payload.reserve(64 + text.size());
    payload += "{\"id\":\"" + json_escape(id) + "\",";
    payload += "\"event\":\"token\",";
    payload += "\"text\":\"" + json_escape(text) + "\",";
    payload += "\"token_id\":" + std::to_string(token_id) + "}";
    write_frame(tx, payload);
}

void append_eos_event(std::vector<uint8_t>& tx, const std::string& id, const std::string& reason) {
    std::string payload = "{\"id\":\"" + json_escape(id) +
                          "\",\"event\":\"eos\",\"reason\":\"" + json_escape(reason) + "\"}";
    write_frame(tx, payload);
}

void append_error_event(std::vector<uint8_t>& tx, const std::string& id, const std::string& code,
                        const std::string& message) {
    std::string payload = "{\"id\":\"" + json_escape(id) + "\",\"event\":\"error\",";
    payload += "\"code\":\"" + json_escape(code) + "\",\"message\":\"" + json_escape(message) + "\"}";
    write_frame(tx, payload);
}

} // namespace uma::ipc::protocol

