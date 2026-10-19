// ITransport поверх неблокирующего файлового дескриптора: сокет, tty (COM-порт), pipe.
#pragma once

#include "transport.hpp"
#include "unique_fd.hpp"

namespace filexfer {

class FdTransport : public ITransport {
public:
    FdTransport(UniqueFd fd, std::string peer) : fd_(std::move(fd)), peer_(std::move(peer)) {}
    int fd() const override { return fd_.get(); }
    ssize_t read(void* buf, size_t n) override;
    ssize_t write(const void* buf, size_t n) override;
    std::string peer() const override { return peer_; }
private:
    UniqueFd fd_;
    std::string peer_;
};

}  // namespace filexfer
