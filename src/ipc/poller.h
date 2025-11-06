#pragma once

#include <cstdint>
#include <sys/types.h>
#include <vector>

namespace uma::ipc {

enum class PollFlags : uint16_t {
    None = 0,
    Read = 1u << 0,
    Write = 1u << 1,
    Hup = 1u << 2,
    Err = 1u << 3
};

// helpers for managing PollFlags
inline PollFlags operator|(PollFlags a, PollFlags b) noexcept {
    return static_cast<PollFlags>(static_cast<uint16_t>(a) | static_cast<uint16_t>(b));
}
inline PollFlags operator&(PollFlags a, PollFlags b) noexcept {
    return static_cast<PollFlags>(static_cast<uint16_t>(a) & static_cast<uint16_t>(b));
}
inline PollFlags& operator|=(PollFlags& a, PollFlags b) noexcept {
    a = a | b;
    return a;
}
inline PollFlags& operator&=(PollFlags& a, PollFlags b) noexcept {
    a = a & b;
    return a;
}
constexpr bool any(PollFlags a) noexcept {
    return static_cast<uint16_t>(a) != 0;
}
constexpr bool has(PollFlags a, PollFlags b) {
    return any(a & b);
}

struct PollEvent {
    int fd = -1;
    PollFlags f = PollFlags::None;

    bool readable() const {
        return has(f, PollFlags::Read);
    }
    bool writable() const {
        return has(f, PollFlags::Write);
    }
    bool hup() const {
        return has(f, PollFlags::Hup);
    }
    bool err() const {
        return has(f, PollFlags::Err);
    }
};

class Poller {
  private:
    int handle_ = -1;

  public:
    Poller();
    ~Poller();

    // non-copyable
    Poller(const Poller&) = delete;
    Poller& operator=(const Poller&) = delete;

    bool add(int fd, PollFlags interest);
    bool remove(int fd, PollFlags interest);

    // blocks up to timeout_ms and fill out events_out with events arrived during this period
    int wait(int timeout_ms, std::vector<PollEvent>& events_out);
};

} // namespace uma::ipc
