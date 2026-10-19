#include "fd_transport.hpp"

#include <cerrno>

namespace filexfer {

ssize_t FdTransport::read(void* buf, size_t n) {
    ssize_t r;
    do { r = ::read(fd_.get(), buf, n); } while (r < 0 && errno == EINTR);
    return r;
}

ssize_t FdTransport::write(const void* buf, size_t n) {
    ssize_t r;
    do { r = ::write(fd_.get(), buf, n); } while (r < 0 && errno == EINTR);
    return r;
}

}  // namespace filexfer
