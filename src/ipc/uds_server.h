// UMA Serve - UDS server (macOS first)
#pragma once

#include <atomic>
#include <functional>
#include <string>

namespace uma::ipc {

class UDSServer {
public:
    using Handler = std::function<void(int client_fd)>;

    UDSServer(std::string path, unsigned mode);
    ~UDSServer();

    // Blocking accept loop; exits when shutdown flag is set
    bool serve(std::atomic<bool>& shutdown_flag, const Handler& handler);

private:
    std::string path_;
    unsigned mode_;
    int listen_fd_ = -1;

    bool open_socket_();
    void close_socket_();
};

} // namespace uma::ipc

