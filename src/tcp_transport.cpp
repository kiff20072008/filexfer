#include "tcp_transport.hpp"

#include "fd_transport.hpp"
#include "log.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace filexfer {

namespace {

constexpr int CONNECT_TIMEOUT_MS = 5000;

std::string sockaddr_to_string(const sockaddr_in& a) {
    char ip[INET_ADDRSTRLEN] = "?";
    inet_ntop(AF_INET, &a.sin_addr, ip, sizeof ip);
    return std::string(ip) + ":" + std::to_string(ntohs(a.sin_port));
}

void set_nodelay(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
}

}  // namespace

// ---------------------------------------------------------------------------
TcpAcceptor::TcpAcceptor(uint16_t port) : port_(port) {
    fd_.reset(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!fd_) throw std::runtime_error(std::string("socket: ") + strerror(errno));
    int one = 1;
    setsockopt(fd_.get(), SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(port);
    if (::bind(fd_.get(), reinterpret_cast<sockaddr*>(&a), sizeof a) < 0 || ::listen(fd_.get(), 64) < 0) {
        std::string e = strerror(errno);
        throw std::runtime_error("bind/listen на порту " + std::to_string(port) + ": " + e);
    }
    spare_.reset(::open("/dev/null", O_RDONLY | O_CLOEXEC));
}

std::unique_ptr<ITransport> TcpAcceptor::accept() {
    for (;;) {
        sockaddr_in a{};
        socklen_t len = sizeof a;
        UniqueFd c(::accept4(fd_.get(), reinterpret_cast<sockaddr*>(&a), &len, SOCK_NONBLOCK | SOCK_CLOEXEC));
        if (c) {
            set_nodelay(c.get());
            return std::make_unique<FdTransport>(std::move(c), sockaddr_to_string(a));
        }
        if (errno == EINTR || errno == ECONNABORTED) continue;
        if ((errno == EMFILE || errno == ENFILE) && spare_) {
            // Кончились дескрипторы. Соединение осталось бы в очереди, слушающий сокет — «читаемым»,
            // и epoll крутился бы вхолостую на 100% CPU. Освобождаем резервный дескриптор,
            // принимаем соединение и сразу закрываем его.
            spare_.reset();
            UniqueFd rejected(::accept(fd_.get(), nullptr, nullptr));
            rejected.reset();
            spare_.reset(::open("/dev/null", O_RDONLY | O_CLOEXEC));
            log_msg("сервер", "Превышен лимит открытых файлов — входящее соединение отклонено");
            continue;
        }
        return nullptr;   // EAGAIN — очередь пуста
    }
}

std::string TcpAcceptor::describe() const { return "TCP порт " + std::to_string(port_); }

// ---------------------------------------------------------------------------
std::unique_ptr<ITransport> TcpConnector::connect(std::string& err) {
    addrinfo hints{}, *res = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(host_.c_str(), std::to_string(port_).c_str(), &hints, &res);
    if (rc != 0) { err = gai_strerror(rc); return nullptr; }
    struct AddrinfoFree { void operator()(addrinfo* p) const { freeaddrinfo(p); } };
    std::unique_ptr<addrinfo, AddrinfoFree> guard(res);

    err = "нет адресов";
    for (addrinfo* ai = res; ai; ai = ai->ai_next) {
        UniqueFd s(::socket(ai->ai_family, ai->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
        if (!s) { err = strerror(errno); continue; }
        int r = ::connect(s.get(), ai->ai_addr, ai->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            pollfd p{s.get(), POLLOUT, 0};
            int pr = poll(&p, 1, CONNECT_TIMEOUT_MS);
            if (pr <= 0) { err = pr < 0 ? strerror(errno) : "таймаут подключения"; continue; }
            int soerr = 0;
            socklen_t sl = sizeof soerr;
            getsockopt(s.get(), SOL_SOCKET, SO_ERROR, &soerr, &sl);
            if (soerr) { err = strerror(soerr); continue; }
        } else if (r < 0) {
            err = strerror(errno);
            continue;
        }
        set_nodelay(s.get());
        return std::make_unique<FdTransport>(std::move(s), sockaddr_to_string(*reinterpret_cast<sockaddr_in*>(ai->ai_addr)));
    }
    return nullptr;
}

std::string TcpConnector::describe() const { return host_ + ":" + std::to_string(port_); }

}  // namespace filexfer
