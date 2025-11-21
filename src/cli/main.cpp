// UMA Serve - CLI client (framed JSON over UDS)
#include "ipc/protocol.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <ctime>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace {

struct CliOptions {
    std::string socket_path = "/tmp/uma.sock";
    std::string id;
    std::string prompt;
    int max_tokens = -1;
    double temperature = 0.0;
    double top_p = 1.0;
    bool stream = true;
    bool metrics = false;
};

void print_usage(const char* argv0) {
    std::cerr << "uma-cli - UMA Serve client (UDS, framed JSON)\n"
              << "Usage: " << argv0
              << " --prompt 'text' [--socket /tmp/uma.sock] [--id req-1] [--max-tokens N] [--temp T] [--top-p P] [--no-stream] [--metrics]\n";
}

std::string gen_default_id() {
    std::ostringstream oss;
    oss << "req-" << ::getpid() << "-" << std::time(nullptr);
    return oss.str();
}

int connect_uds(const std::string& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        std::perror("socket");
        return -1;
    }
    sockaddr_un su{};
    su.sun_family = AF_UNIX;
    if (path.size() >= sizeof(su.sun_path)) {
        std::cerr << "socket path too long\n";
        ::close(fd);
        return -1;
    }
    std::strncpy(su.sun_path, path.c_str(), sizeof(su.sun_path) - 1);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&su), sizeof(su)) != 0) {
        std::perror("connect");
        ::close(fd);
        return -1;
    }
    return fd;
}

bool send_all(int fd, const uint8_t* data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t n = ::write(fd, data + off, len - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) { ::usleep(1000); continue; }
            return false;
        }
        off += (size_t)n;
    }
    return true;
}

// Very small JSON value extractor for event fields we care about
std::string json_get_string(const std::string& j, const char* key) {
    std::string k = std::string("\"") + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return {};
    size_t colon = j.find(':', p);
    if (colon == std::string::npos) return {};
    size_t q1 = j.find('"', colon);
    if (q1 == std::string::npos) return {};
    size_t i = q1 + 1;
    std::string out;
    while (i < j.size()) {
        char c = j[i++];
        if (c == '\\') {
            if (i >= j.size()) break;
            char e = j[i++];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                default: out.push_back(e); break;
            }
        } else if (c == '"') {
            break;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

} // namespace

int main(int argc, char** argv) {
    CliOptions opt;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](int i) { return i + 1 < argc; };
        if (a == "--help" || a == "-h") { print_usage(argv[0]); return 0; }
        else if (a == "--socket" && need(i)) { opt.socket_path = argv[++i]; }
        else if (a == "--prompt" && need(i)) { opt.prompt = argv[++i]; }
        else if (a == "--id" && need(i)) { opt.id = argv[++i]; }
        else if (a == "--max-tokens" && need(i)) { opt.max_tokens = std::atoi(argv[++i]); }
        else if (a == "--temp" && need(i)) { opt.temperature = std::atof(argv[++i]); }
        else if (a == "--top-p" && need(i)) { opt.top_p = std::atof(argv[++i]); }
        else if (a == "--no-stream") { opt.stream = false; }
        else if (a == "--metrics") { opt.metrics = true; }
        else {
            std::cerr << "Unknown or incomplete flag: " << a << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!opt.metrics && opt.prompt.empty()) {
        std::cerr << "--prompt is required (or use --metrics)\n";
        print_usage(argv[0]);
        return 1;
    }
    if (opt.id.empty()) opt.id = gen_default_id();

    int fd = connect_uds(opt.socket_path);
    if (fd < 0) return 3; // protocol/connect error

    std::string payload;
    if (opt.metrics) {
        payload = "{\"type\":\"metrics\"}";
    } else {
        // Build request JSON (minimal manual serialization)
        payload.reserve(opt.prompt.size() + 128);
        payload += "{\"id\":\"" + uma::ipc::protocol::json_escape(opt.id) + "\",";
        payload += "\"prompt\":\"" + uma::ipc::protocol::json_escape(opt.prompt) + "\",";
        payload += std::string("\"stream\":") + (opt.stream ? "true" : "false");
        if (opt.max_tokens > 0) payload += ",\"max_tokens\":" + std::to_string(opt.max_tokens);
        // temperature/top_p are optional and may be ignored server-side for now
        payload += ",\"temperature\":" + std::to_string(opt.temperature);
        payload += ",\"top_p\":" + std::to_string(opt.top_p);
        payload += "}";
    }

    // Frame and send
    std::vector<uint8_t> tx;
    uma::ipc::protocol::write_frame(tx, payload);
    if (!send_all(fd, tx.data(), tx.size())) {
        std::perror("send");
        ::close(fd);
        return 3;
    }

    // Receive and print events until eos or error (or metrics one-shot)
    while (true) {
        uint8_t hdr[4];
        ssize_t n = ::read(fd, hdr, 4);
        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            std::perror("read");
            ::close(fd);
            return 3;
        }
        if (n != 4) { std::cerr << "short header read\n"; break; }
        uint32_t len = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) | ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
        std::string js;
        js.resize(len);
        size_t off = 0;
        while (off < len) {
            ssize_t r = ::read(fd, &js[off], len - off);
            if (r <= 0) { if (errno == EINTR) continue; break; }
            off += (size_t)r;
        }
        if (off != len) { std::cerr << "short payload read\n"; break; }

        // Metrics one-shot
        if (opt.metrics) {
            std::cout << js << std::endl;
            ::close(fd);
            return 0;
        }

        std::string event = json_get_string(js, "event");
        if (event == "token") {
            std::string text = json_get_string(js, "text");
            if (!text.empty()) std::cout << text << std::flush;
        } else if (event == "eos") {
            std::cout << std::endl;
            ::close(fd);
            return 0;
        } else if (event == "error") {
            std::string msg = json_get_string(js, "message");
            if (msg.empty()) msg = js;
            std::cerr << msg << std::endl;
            ::close(fd);
            return 2; // server error
        } else if (event == "metrics") {
            // If server sent metrics as an event on normal connection, print it
            std::cout << js << std::endl;
            // keep connection open; allow subsequent request if needed
            ::close(fd);
            return 0;
        } else {
            // Unknown event; print raw
            std::cout << js << std::endl;
        }
    }

    ::close(fd);
    return 0;
}

