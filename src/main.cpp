// filexfer — передача файла между двумя процессами (клиент и сервер в одном исполняемом файле).
//
// Сервер:   ./filexfer -s [порт]                         (по умолчанию 5555)
// Клиент:   ./filexfer -c <путь_к_файлу> [хост] [порт]   (по умолчанию 127.0.0.1:5555)
//
// Архитектура (снизу вверх):
//   transport.hpp       ITransport / IAcceptor / IConnector — абстракция канала (неблокирующий поток байт)
//   tcp_transport       TCP-реализация; COM-порт добавляется новым классом поверх FdTransport
//   fault_transport     декоратор с потерей/искажением байтов — для проверки протокола на плохом канале
//   protocol            кадрирование (заголовок + CRC32), буферы, ресинхронизация
//   server              epoll: много клиентов одновременно, 0% CPU в ожидании
//   client              отправка с окном, повторами и докачкой после обрыва

#include "client.hpp"
#include "fault_transport.hpp"
#include "log.hpp"
#include "server.hpp"
#include "tcp_transport.hpp"

#include <signal.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

using namespace filexfer;

namespace {

constexpr unsigned DEFAULT_PORT = 5555;

void usage(const char* prog) {
    std::fprintf(stderr,
                 "Использование:\n"
                 "  %s -s [порт]                      сервер (по умолчанию порт %u)\n"
                 "  %s -c <файл> [хост] [порт]        клиент (по умолчанию 127.0.0.1:%u)\n"
                 "\n"
                 "Для тестов клиент может имитировать канал с помехами:\n"
                 "  FILEXFER_FAULTS=\"drop=0.0001,flip=0.0001,seed=1\" %s -c <файл>\n",
                 prog, DEFAULT_PORT, prog, DEFAULT_PORT, prog);
}

bool parse_port(const char* s, uint16_t& out) {
    char* end = nullptr;
    long v = strtol(s, &end, 10);
    if (!*s || *end || v < 1 || v > 65535) return false;
    out = static_cast<uint16_t>(v);
    return true;
}

// «Папка с программой» — каталог исполняемого файла, а не текущий каталог
std::string program_dir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    std::string p(buf);
    size_t s = p.rfind('/');
    return s == std::string::npos ? "." : (s == 0 ? "/" : p.substr(0, s));
}

int server_main(int argc, char** argv) {
    uint16_t port = DEFAULT_PORT;
    if (argc > 2 && !parse_port(argv[2], port)) { usage(argv[0]); return 2; }
    std::string dir = program_dir();
    if (access(dir.c_str(), W_OK) != 0) {
        std::fprintf(stderr, "Нет прав на запись в каталог программы: %s\n", dir.c_str());
        return 1;
    }
    TcpAcceptor acceptor(port);          // другой интерфейс подключается здесь
    return run_server(acceptor, dir);
}

int client_main(int argc, char** argv) {
    if (argc < 3) { usage(argv[0]); return 2; }
    std::string host = argc > 3 ? argv[3] : "127.0.0.1";
    uint16_t port = DEFAULT_PORT;
    if (argc > 4 && !parse_port(argv[4], port)) { usage(argv[0]); return 2; }
    TcpConnector tcp(host, port);        // и здесь

    if (const char* env = std::getenv("FILEXFER_FAULTS"); env && *env) {
        FaultSpec spec;
        std::string err;
        if (!parse_fault_spec(env, spec, err)) {
            std::fprintf(stderr, "FILEXFER_FAULTS: %s\n", err.c_str());
            return 2;
        }
        log_msg("клиент", "Имитация помех канала: %s", describe(spec).c_str());
        FaultyConnector noisy(tcp, spec);
        return run_client(noisy, argv[2]);
    }
    return run_client(tcp, argv[2]);
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    if (argc < 2) { usage(argv[0]); return 2; }
    std::string mode = argv[1];
    try {
        if (mode == "-s") return server_main(argc, argv);
        // «-с» в задании набрано кириллической «с» — принимаем оба варианта
        if (mode == "-c" || mode == "-\xD1\x81") return client_main(argc, argv);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Ошибка: %s\n", e.what());
        return 1;
    }
    usage(argv[0]);
    return 2;
}
