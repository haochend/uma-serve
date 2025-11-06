#include <cstddef>
#include <stdexcept>
#include <sys/event.h>
#include <unistd.h>
#include <unordered_map>

#include "ipc/poller.h"

namespace uma::ipc {

Poller::Poller() {
    handle_ = ::kqueue();
    if (handle_ < 0) {
        throw std::runtime_error("failed to create kqueue");
    }
}

Poller::~Poller() {
    if (handle_ > 0)
        ::close(handle_);
}

bool Poller::add(int fd, PollFlags interest) {
    struct kevent evs[2];
    int n = 0;

    if (has(interest, PollFlags::Read)) {
        EV_SET(&evs[n++], fd, EVFILT_READ, EV_ADD, 0, 0, nullptr);
    }
    if (has(interest, PollFlags::Write)) {
        EV_SET(&evs[n++], fd, EVFILT_WRITE, EV_ADD, 0, 0, nullptr);
    }
    if (n == 0)
        return true; // nothing to add

    if (::kevent(handle_, evs, n, nullptr, 0, nullptr) == -1) {
        return false; // or throw with strerror(errno)
    }
    return true;
}

bool Poller::remove(int fd, PollFlags interest) {
    struct kevent evs[2];
    int n = 0;

    if (has(interest, PollFlags::Read)) {
        EV_SET(&evs[n++], fd, EVFILT_READ, EV_DELETE, 0, 0, nullptr);
    }
    if (has(interest, PollFlags::Write)) {
        EV_SET(&evs[n++], fd, EVFILT_WRITE, EV_DELETE, 0, 0, nullptr);
    }
    if (n == 0)
        return true; // nothing to delete

    if (::kevent(handle_, evs, n, nullptr, 0, nullptr) == -1) {
        if (errno == ENOENT || errno == EBADF)
            return true; // benign
        return false;    // or throw
    }
    return true;
}

int Poller::wait(int timeout_ms, std::vector<PollEvent>& events_out) {
    events_out.clear();

    timespec ts, *tsp = nullptr;
    if (timeout_ms >= 0) {
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000;
        tsp = &ts;
    }

    struct kevent kev[64];
    int nev = ::kevent(handle_, nullptr, 0, kev, 64, tsp);
    if (nev <= 0)
        return nev;

    // coalesce by fd
    std::unordered_map<int, PollFlags> acc;
    acc.reserve((size_t)nev);

    for (int i = 0; i < nev; ++i) {
        int fd = (int)kev[i].ident;
        PollFlags f = PollFlags::None;
        if (kev[i].filter == EVFILT_READ)
            f |= PollFlags::Read;
        if (kev[i].filter == EVFILT_WRITE)
            f |= PollFlags::Write;
        if (kev[i].flags & EV_ERROR)
            f |= PollFlags::Err;
        if (kev[i].flags & EV_EOF)
            f |= PollFlags::Hup;
        acc[fd] = acc.count(fd) ? (acc[fd] | f) : f;
    }

    events_out.reserve(acc.size());
    for (auto& kv : acc)
        events_out.push_back({kv.first, kv.second});
    return (int)events_out.size();
}

} // namespace uma::ipc
