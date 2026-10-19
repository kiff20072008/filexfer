// Общие утилиты: время, контрольные суммы, сериализация. Без зависимостей от ОС.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace filexfer {

int64_t now_ms();   // монотонное время, мс

uint32_t crc32(const void* data, size_t n, uint32_t crc = 0);   // crc можно передавать для продолжения
uint64_t fnv1a64(const void* data, size_t n, uint64_t h = 0xCBF29CE484222325ull);

// Имя файла от клиента попадает в лог — управляющие символы заменяем, чтобы не испортить терминал.
std::string printable(const std::string& s);

// Сериализация. Все числа в протоколе — big-endian.
class Writer {
public:
    Writer& u8(uint8_t v)   { return put(v, 1); }
    Writer& u16(uint16_t v) { return put(v, 2); }
    Writer& u32(uint32_t v) { return put(v, 4); }
    Writer& u64(uint64_t v) { return put(v, 8); }
    Writer& str(const std::string& s) {
        size_t n = std::min<size_t>(s.size(), 65535);
        u16(static_cast<uint16_t>(n));
        buf_.insert(buf_.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>(n));
        return *this;
    }
    const std::vector<uint8_t>& data() const { return buf_; }
private:
    Writer& put(uint64_t v, int k) {
        for (int i = k - 1; i >= 0; --i) buf_.push_back(static_cast<uint8_t>(v >> (8 * i)));
        return *this;
    }
    std::vector<uint8_t> buf_;
};

// Чтение с проверкой границ: при выходе за конец ok() становится false, значения — нули.
class Reader {
public:
    Reader(const uint8_t* p, size_t n) : p_(p), n_(n) {}
    uint8_t  u8()  { return static_cast<uint8_t>(take(1)); }
    uint16_t u16() { return static_cast<uint16_t>(take(2)); }
    uint32_t u32() { return static_cast<uint32_t>(take(4)); }
    uint64_t u64() { return take(8); }
    std::string str() {
        size_t len = u16();
        if (!ok_ || n_ < len) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(p_), len);
        p_ += len; n_ -= len;
        return s;
    }
    const uint8_t* rest() const { return p_; }
    size_t left() const { return n_; }
    bool ok() const { return ok_; }
private:
    uint64_t take(size_t k) {
        if (n_ < k) { ok_ = false; n_ = 0; return 0; }
        uint64_t v = 0;
        for (size_t i = 0; i < k; ++i) v = (v << 8) | p_[i];
        p_ += k; n_ -= k;
        return v;
    }
    const uint8_t* p_;
    size_t n_;
    bool ok_ = true;
};

}  // namespace filexfer
