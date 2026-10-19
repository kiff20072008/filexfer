// Сервер: принимает файлы от любого числа клиентов одновременно (epoll, один поток).
#pragma once

#include "transport.hpp"

#include <string>

namespace filexfer {

// Работает до SIGINT/SIGTERM. Полученные файлы сохраняются в dir как ГГГГММДД_ЧЧММСС.hex.
// Возвращает код завершения процесса; при фатальной ошибке бросает std::runtime_error.
int run_server(IAcceptor& acceptor, const std::string& dir);

}  // namespace filexfer
