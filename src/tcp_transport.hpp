// TCP-реализация транспорта (IPv4).
#pragma once

#include "transport.hpp"
#include "unique_fd.hpp"

#include <cstdint>

namespace filexfer {

class TcpAcceptor : public IAcceptor {
public:
    explicit TcpAcceptor(uint16_t port);   // bind + listen на всех интерфейсах; бросает std::runtime_error
    int fd() const override { return fd_.get(); }
    std::unique_ptr<ITransport> accept() override;
    std::string describe() const override;
private:
    UniqueFd fd_;
    UniqueFd spare_;   // резервный дескриптор на случай EMFILE, см. accept()
    uint16_t port_;
};

class TcpConnector : public IConnector {
public:
    TcpConnector(std::string host, uint16_t port) : host_(std::move(host)), port_(port) {}
    std::unique_ptr<ITransport> connect(std::string& err) override;
    std::string describe() const override;
private:
    std::string host_;
    uint16_t port_;
};

}  // namespace filexfer
