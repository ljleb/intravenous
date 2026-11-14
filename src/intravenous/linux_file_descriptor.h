#pragma once

#if !defined(__linux__)
#error "Intravenous filesystem watching requires Linux"
#endif

#include <cerrno>
#include <cstdint>
#include <string>
#include <system_error>
#include <utility>

#include <sys/eventfd.h>
#include <unistd.h>

namespace iv {
[[noreturn]] inline void throw_errno(char const* operation)
{
    throw std::system_error(errno, std::generic_category(), operation);
}

class UniqueFileDescriptor {
    int fd_ = -1;

public:
    UniqueFileDescriptor() noexcept = default;
    explicit UniqueFileDescriptor(int fd) noexcept
        : fd_(fd)
    {}

    ~UniqueFileDescriptor()
    {
        reset();
    }

    UniqueFileDescriptor(UniqueFileDescriptor const&) = delete;
    UniqueFileDescriptor& operator=(UniqueFileDescriptor const&) = delete;

    UniqueFileDescriptor(UniqueFileDescriptor&& other) noexcept
        : fd_(std::exchange(other.fd_, -1))
    {}

    UniqueFileDescriptor& operator=(UniqueFileDescriptor&& other) noexcept
    {
        if (this != &other) reset(std::exchange(other.fd_, -1));
        return *this;
    }

    [[nodiscard]] int get() const noexcept { return fd_; }
    [[nodiscard]] explicit operator bool() const noexcept { return fd_ != -1; }

    void reset(int fd = -1) noexcept
    {
        if (fd_ != -1) (void)close(fd_);
        fd_ = fd;
    }
};

class EventSignal {
    UniqueFileDescriptor fd_;
    std::string label_;

    static UniqueFileDescriptor open(std::string const& label)
    {
        auto const fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd == -1) {
            throw std::system_error(
                errno, std::generic_category(), "eventfd(" + label + ")");
        }
        return UniqueFileDescriptor(fd);
    }

public:
    explicit EventSignal(std::string label)
        : fd_(open(label))
        , label_(std::move(label))
    {}

    [[nodiscard]] int native_handle() const noexcept { return fd_.get(); }

    void signal() noexcept
    {
        std::uint64_t const value = 1;
        ssize_t result = -1;
        do {
            result = write(fd_.get(), &value, sizeof(value));
        } while (result == -1 && errno == EINTR);
        // EAGAIN means the counter is already saturated, which still leaves the
        // descriptor readable and therefore preserves the wakeup.
    }

    void consume()
    {
        std::uint64_t value = 0;
        for (;;) {
            auto const count = read(fd_.get(), &value, sizeof(value));
            if (count == static_cast<ssize_t>(sizeof(value))) continue;
            if (count == -1 && errno == EINTR) continue;
            if (count == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            if (count == 0) return;
            if (count == -1) {
                throw std::system_error(
                    errno,
                    std::generic_category(),
                    "read(eventfd " + label_ + ")");
            }
        }
    }
};
} // namespace iv
