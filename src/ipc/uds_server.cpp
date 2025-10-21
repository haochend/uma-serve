// UMA Serve - UDS server (macOS first)
#include "ipc/uds_server.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace uma::ipc {

UDSServer::UDSServer(std::string path, unsigned mode)
    : path_(std::move(path)), mode_(mode) {}

UDSServer::~UDSServer() {
    close_socket_();
    if (!path_.empty()) {
        ::unlink(path_.c_str());
    }
}

bool UDSServer::open_socket_() {
    // Remove stale path
    ::unlink(path_.c_str());

    listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        std::perror("socket");
        return false;
    }

    // close-on-exec
    int flags = ::fcntl(listen_fd_, F_GETFD);
    if (flags != -1) ::fcntl(listen_fd_, F_SETFD, flags | FD_CLOEXEC);

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (path_.size() >= sizeof(addr.sun_path)) {
        std::cerr << "UDS path too long: " << path_ << "\n";
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }
    std::strncpy(addr.sun_path, path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::perror("bind");
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    // Secure perms
    if (::chmod(path_.c_str(), static_cast<mode_t>(mode_)) < 0) {
        std::perror("chmod");
        // continue anyway
    }

    if (::listen(listen_fd_, 16) < 0) {
        std::perror("listen");
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    std::cout << "UDS listening at " << path_ << " (mode " << std::oct << mode_ << std::dec << ")\n";
    return true;
}

void UDSServer::close_socket_() {
    if (listen_fd_ >= 0) {
        ::close(listen_fd_);
        listen_fd_ = -1;
    }
}

bool UDSServer::serve(std::atomic<bool>& shutdown_flag, const Handler& handler) {
    if (!open_socket_()) return false;

    while (!shutdown_flag.load(std::memory_order_relaxed)) {
        int cfd = ::accept(listen_fd_, nullptr, nullptr);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue; // check shutdown flag
            }
            std::perror("accept");
            break;
        }

        handler(cfd);
        ::close(cfd);
    }

    close_socket_();
    ::unlink(path_.c_str());
    return true;
}

} // namespace uma::ipc

