// Вывод сообщений: «[ЧЧ:ММ:СС] [роль] текст» и строка прогресса, обновляемая через \r.
#pragma once

namespace filexfer {

void log_msg(const char* role, const char* fmt, ...) __attribute__((format(printf, 2, 3)));

// Перерисовывает текущую строку; следующий log_msg сначала переведёт строку.
void log_progress(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

}  // namespace filexfer
