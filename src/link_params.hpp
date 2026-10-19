// Параметры канала: размер блока, окно и таймауты.
//
// Протокол один и тот же для любого интерфейса, но подходящие числа зависят от скорости линии:
// по TCP окно в 512 КиБ уходит за миллисекунды, а по COM-порту на 115200 бод — за 45 секунд.
// Поэтому параметры задаёт транспорт (IAcceptor/IConnector::params()), а клиент и сервер их только читают.
#pragma once

#include <cstddef>
#include <cstdint>

namespace filexfer {

struct LinkParams {
    // Клиент
    size_t   max_chunk      = 32 * 1024;   // наибольший блок данных в одном кадре
    size_t   min_chunk      = 512;         // при потерях блок уменьшается вдвое, но не ниже этого
    uint64_t window_bytes   = 512 * 1024;  // сколько байт может быть «в полёте» без подтверждения
    int64_t  retransmit_ms  = 3'000;       // нет подтверждений -> повторить с последнего подтверждённого байта
    int64_t  peer_dead_ms   = 15'000;      // нет кадров от сервера -> переподключиться

    // Сервер
    int64_t  idle_ms        = 30'000;      // тишина от клиента -> закрыть соединение
    int64_t  resume_ttl_ms  = 60'000;      // сколько ждать возврата клиента после обрыва
    int64_t  renack_ms      = 200;         // не чаще одного повторного NACK за этот интервал
};

// Значения для TCP (по умолчанию)
inline LinkParams tcp_link_params() { return LinkParams{}; }

// Значения для последовательного порта: окно — около двух секунд передачи на этой скорости,
// таймауты — с запасом относительно времени передачи окна.
inline LinkParams serial_link_params(unsigned baud) {
    LinkParams p;
    uint64_t bytes_per_sec = baud / 10 ? baud / 10 : 1;               // 8N1: 10 бит на байт
    auto clamp = [](uint64_t v, uint64_t lo, uint64_t hi) { return v < lo ? lo : (v > hi ? hi : v); };
    p.window_bytes  = clamp(bytes_per_sec * 2, 4 * 1024, 512 * 1024);
    p.max_chunk     = static_cast<size_t>(clamp(bytes_per_sec / 8, 256, 32 * 1024));
    p.min_chunk     = p.max_chunk < 512 ? p.max_chunk : 512;
    int64_t window_ms = static_cast<int64_t>(p.window_bytes * 1000 / bytes_per_sec);
    p.retransmit_ms = 3'000 + 2 * window_ms;
    p.peer_dead_ms  = 5 * p.retransmit_ms;
    p.idle_ms       = 2 * p.peer_dead_ms;
    p.resume_ttl_ms = 2 * p.idle_ms;
    p.renack_ms     = 200 + window_ms / 4;
    return p;
}

}  // namespace filexfer
