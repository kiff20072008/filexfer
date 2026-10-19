// Протокол передачи и кадрирование поверх любого ITransport.
//
// Кадр:  magic(2) type(1) version(1) len(4) crc_payload(4) crc_header(4) | payload
//   crc_header — CRC32 первых 12 байт заголовка, поэтому в потоке без гарантий (COM-порт)
//   можно искать границу кадра после мусора/потери байт.
//
// Сообщения (числа big-endian, str = длина(2) + байты):
//   START      C->S  id(8) size(8) flags(1) name(str) — «хочу передать файл»
//                    id = хеш(имя, размер, mtime, CRC файла): перезапущенный клиент докачивает с места обрыва.
//                    flags & START_RECONNECT — переподключение того же процесса (сервер может ответить ALREADY_DONE)
//   START_ACK  S->C  id(8) offset(8) status(1) name(str) — с какого байта продолжать;
//                    id может быть заменён сервером (параллельная отправка того же файла), клиент его запоминает
//   DATA       C->S  offset(8) bytes...
//   ACK        S->C  next_offset(8)                — кумулятивное подтверждение
//   NACK       S->C  expected_offset(8)            — потеря/порча: «повтори с этого места»
//   END        C->S  size(8) crc32_file(4)
//   DONE       S->C  status(1) name(str)
#pragma once

#include "transport.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace filexfer {

constexpr uint16_t MAGIC       = 0xA55A;
constexpr uint8_t  VERSION     = 2;
constexpr size_t   HEADER      = 16;
constexpr size_t   MAX_PAYLOAD = 1 << 20;

constexpr uint8_t START_RECONNECT = 0x01;

enum MsgType : uint8_t { MSG_START = 1, MSG_START_ACK, MSG_DATA, MSG_ACK, MSG_NACK, MSG_END, MSG_DONE };
enum Status : uint8_t { ST_OK = 0, ST_ALREADY_DONE = 1, ST_BAD_CRC = 2, ST_BAD_SIZE = 3, ST_IO_ERROR = 4, ST_PROTOCOL = 5 };

const char* status_text(uint8_t s);

struct Frame {
    uint8_t type = 0;
    std::vector<uint8_t> payload;
};

// Буферизованное соединение: кадры туда и обратно поверх любого ITransport.
class Connection {
public:
    enum class Next { Frame, Need };

    explicit Connection(std::unique_ptr<ITransport> t) : t_(std::move(t)) {}
    int fd() const { return t_->fd(); }
    std::string peer() const { return t_->peer(); }

    // false — соединение закрыто/сломано (уже прочитанные данные при этом остаются в буфере)
    bool pump_read();
    bool pump_write();
    bool has_output() const { return out_pos_ < out_.size(); }

    void queue(uint8_t type, const std::vector<uint8_t>& payload) { queue(type, payload.data(), payload.size()); }
    // Кадр из двух частей (заголовок сообщения + данные) без лишнего копирования в промежуточный буфер
    void queue(uint8_t type, const void* a, size_t an, const void* b = nullptr, size_t bn = 0);

    // Достаёт следующий корректный кадр. Мусор и кадры с битой CRC пропускаются (ресинхронизация).
    Next next(Frame& f);

    // Были ли с прошлого вызова пропущены испорченные байты/кадры
    bool take_corrupt() { bool c = corrupted_; corrupted_ = false; return c; }

private:
    void compact();

    std::unique_ptr<ITransport> t_;
    std::vector<uint8_t> in_, out_;
    size_t in_pos_ = 0, out_pos_ = 0;
    bool corrupted_ = false;
};

}  // namespace filexfer
