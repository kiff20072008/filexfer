#include "protocol.hpp"

#include "util.hpp"

#include <cerrno>

namespace filexfer {

namespace {

void put32(uint8_t* p, uint32_t v) {
    for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}

uint32_t get32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

}  // namespace

const char* status_text(uint8_t s) {
    switch (s) {
        case ST_OK: return "ok";
        case ST_ALREADY_DONE: return "уже принят";
        case ST_BAD_CRC: return "контрольная сумма файла не совпала";
        case ST_BAD_SIZE: return "размер не совпал";
        case ST_IO_ERROR: return "ошибка записи на сервере";
        default: return "ошибка протокола";
    }
}

bool Connection::pump_read() {
    uint8_t buf[65536];
    for (int i = 0; i < 8; ++i) {   // ограничение, чтобы один клиент не занимал цикл событий
        ssize_t n = t_->read(buf, sizeof buf);
        if (n > 0) {
            in_.insert(in_.end(), buf, buf + n);
            if (static_cast<size_t>(n) < sizeof buf) return true;
            continue;
        }
        if (n == 0) return false;
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
    return true;
}

bool Connection::pump_write() {
    while (out_pos_ < out_.size()) {
        ssize_t n = t_->write(out_.data() + out_pos_, out_.size() - out_pos_);
        if (n > 0) { out_pos_ += static_cast<size_t>(n); continue; }
        if (n == 0) return true;
        return errno == EAGAIN || errno == EWOULDBLOCK;
    }
    out_.clear();
    out_pos_ = 0;
    return true;
}

void Connection::queue(uint8_t type, const void* a, size_t an, const void* b, size_t bn) {
    uint8_t h[HEADER];
    uint32_t pcrc = crc32(a, an);
    if (bn) pcrc = crc32(b, bn, pcrc);
    h[0] = MAGIC >> 8;
    h[1] = MAGIC & 0xFF;
    h[2] = type;
    h[3] = VERSION;
    put32(h + 4, static_cast<uint32_t>(an + bn));
    put32(h + 8, pcrc);
    put32(h + 12, crc32(h, 12));
    out_.insert(out_.end(), h, h + HEADER);
    const uint8_t* pa = static_cast<const uint8_t*>(a);
    out_.insert(out_.end(), pa, pa + an);
    if (bn) {
        const uint8_t* pb = static_cast<const uint8_t*>(b);
        out_.insert(out_.end(), pb, pb + bn);
    }
}

Connection::Next Connection::next(Frame& f) {
    for (;;) {
        size_t avail = in_.size() - in_pos_;
        if (avail < HEADER) { compact(); return Next::Need; }
        const uint8_t* h = in_.data() + in_pos_;
        bool ok = h[0] == (MAGIC >> 8) && h[1] == (MAGIC & 0xFF) && h[3] == VERSION &&
                  get32(h + 12) == crc32(h, 12) && get32(h + 4) <= MAX_PAYLOAD;
        if (!ok) { ++in_pos_; corrupted_ = true; continue; }
        size_t len = get32(h + 4);
        if (avail < HEADER + len) { compact(); return Next::Need; }
        if (crc32(h + HEADER, len) != get32(h + 8)) {
            in_pos_ += HEADER + len;
            corrupted_ = true;
            continue;
        }
        f.type = h[2];
        f.payload.assign(h + HEADER, h + HEADER + len);
        in_pos_ += HEADER + len;
        return Next::Frame;
    }
}

void Connection::compact() {
    if (in_pos_) {
        in_.erase(in_.begin(), in_.begin() + static_cast<std::ptrdiff_t>(in_pos_));
        in_pos_ = 0;
    }
}

}  // namespace filexfer
