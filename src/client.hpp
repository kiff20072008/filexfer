// Клиент: отправляет один файл и завершается. Переподключается и докачивает после обрывов.
#pragma once

#include "transport.hpp"

#include <string>

namespace filexfer {

// Возвращает код завершения процесса: 0 — файл принят сервером и проверен, 1 — ошибка,
// 128+сигнал — прервано пользователем (Ctrl+C / SIGTERM).
int run_client(IConnector& connector, const std::string& path);

}  // namespace filexfer
