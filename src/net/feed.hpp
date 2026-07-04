#pragma once

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "../mem/arena.hpp"
#include "../ring_buffer.hpp"
#include "../runtime/streaming.hpp"

namespace aegis::net {

struct WireTick {
    uint64_t ts_ns;
    double price;
    double volume;
    uint64_t sequence;
};
static_assert(sizeof(WireTick) == 32);

struct CopyStats {
    uint64_t socket_to_arena_bytes = 0;
    uint64_t arena_to_ring_bytes = 0;
    uint64_t frames = 0;
    uint64_t partial_frame_copies = 0;
};

class TcpFeed {
public:
    TcpFeed(const std::string& host, uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) throw std::runtime_error("feed socket failed");
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1 ||
            ::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            ::close(fd_);
            fd_ = -1;
            throw std::runtime_error("feed connect failed");
        }
    }
    ~TcpFeed() { if (fd_ >= 0) ::close(fd_); }
    TcpFeed(const TcpFeed&) = delete;
    TcpFeed& operator=(const TcpFeed&) = delete;

    template <size_t Capacity>
    size_t receive(mem::Arena& arena,
                   runtime::BoundedChannel<Tick, Capacity>& channel,
                   size_t max_frames = 1024) {
        const size_t capacity = max_frames * sizeof(WireTick);
        auto* buffer = arena.allocate_n<std::byte>(capacity + sizeof(WireTick));
        std::memcpy(buffer, partial_.data(), partial_size_);
        iovec iov{buffer + partial_size_, capacity};
        const ssize_t received = ::readv(fd_, &iov, 1);  // copy #1
        if (received == 0) return 0;
        if (received < 0) {
            if (errno == EINTR) return 0;
            throw std::runtime_error("feed readv failed");
        }
        stats_.socket_to_arena_bytes += static_cast<uint64_t>(received);
        const size_t bytes = partial_size_ + static_cast<size_t>(received);
        const size_t frames = bytes / sizeof(WireTick);
        const auto* wire = reinterpret_cast<const WireTick*>(buffer);  // cast: copy #0
        size_t accepted = 0;
        for (size_t i = 0; i < frames; ++i) {
            const Tick tick{wire[i].ts_ns, wire[i].price, wire[i].volume};
            if (channel.push(tick)) {  // copy #2: Tick into bounded ring slot
                ++accepted;
                stats_.arena_to_ring_bytes += sizeof(Tick);
            }
        }
        partial_size_ = bytes - frames * sizeof(WireTick);
        if (partial_size_) {
            std::memcpy(partial_.data(), buffer + frames * sizeof(WireTick), partial_size_);
            ++stats_.partial_frame_copies;
        }
        stats_.frames += frames;
        return accepted;
    }
    const CopyStats& stats() const { return stats_; }

private:
    int fd_ = -1;
    std::array<std::byte, sizeof(WireTick)> partial_{};
    size_t partial_size_ = 0;
    CopyStats stats_;
};

}  // namespace aegis::net
