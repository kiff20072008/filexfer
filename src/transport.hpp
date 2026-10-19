// Абстракция канала связи.
//
// Любой интерфейс (TCP, COM-порт, …) — это «неблокирующий поток байт», у которого есть
// файловый дескриптор для poll/epoll. Чтобы добавить COM-порт, достаточно:
//   open("/dev/ttyUSB0", O_RDWR|O_NOCTTY|O_NONBLOCK) + termios (raw, скорость) -> FdTransport,
//   и обернуть в IConnector (клиент) / IAcceptor (сервер, отдаёт единственное «соединение»).
// Протокол и логика клиента/сервера при этом не меняются.
#pragma once

#include "link_params.hpp"

#include <sys/types.h>

#include <memory>
#include <string>

namespace filexfer {

class ITransport {
public:
    virtual ~ITransport() = default;
    virtual int fd() const = 0;                              // дескриптор для poll/epoll
    virtual ssize_t read(void* buf, size_t n) = 0;           // как read(2): -1/EAGAIN — данных нет; 0 — закрыто
    virtual ssize_t write(const void* buf, size_t n) = 0;    // как write(2)
    virtual std::string peer() const = 0;                    // для логов
};

class IAcceptor {  // сервер: источник входящих соединений
public:
    virtual ~IAcceptor() = default;
    virtual int fd() const = 0;                              // читаемость = есть новое соединение
    virtual std::unique_ptr<ITransport> accept() = 0;        // nullptr — нет ожидающих
    virtual std::string describe() const = 0;
    virtual LinkParams params() const { return tcp_link_params(); }   // таймауты под скорость канала
};

class IConnector {  // клиент: создаёт соединение
public:
    virtual ~IConnector() = default;
    virtual std::unique_ptr<ITransport> connect(std::string& err) = 0;
    virtual std::string describe() const = 0;
    virtual LinkParams params() const { return tcp_link_params(); }
};

}  // namespace filexfer
