#include "fault_transport.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace filexfer {

bool parse_fault_spec(const std::string& text, FaultSpec& out, std::string& err) {
    FaultSpec spec;
    std::stringstream ss(text);
    std::string item;
    bool any = false;
    while (std::getline(ss, item, ',')) {
        size_t eq = item.find('=');
        if (eq == std::string::npos) { err = "ожидалось ключ=значение: '" + item + "'"; return false; }
        std::string key = item.substr(0, eq), val = item.substr(eq + 1);
        const char* begin = val.c_str();
        char* end = nullptr;
        if (key == "drop" || key == "flip") {
            double p = std::strtod(begin, &end);
            if (val.empty() || *end || !(p >= 0.0 && p <= 1.0)) { err = key + ": нужна вероятность от 0 до 1"; return false; }
            (key == "drop" ? spec.drop : spec.flip) = p;
        } else if (key == "dir") {
            if (val == "tx") { spec.tx = true; spec.rx = false; }
            else if (val == "rx") { spec.tx = false; spec.rx = true; }
            else if (val == "both") { spec.tx = spec.rx = true; }
            else { err = "dir: нужно tx, rx или both"; return false; }
        } else if (key == "seed") {
            unsigned long long s = std::strtoull(begin, &end, 10);
            if (val.empty() || *end) { err = "seed: нужно целое число"; return false; }
            spec.seed = s;
        } else {
            err = "неизвестный параметр '" + key + "'";
            return false;
        }
        any = true;
    }
    if (!any) { err = "пустая строка"; return false; }
    out = spec;
    return true;
}

std::string describe(const FaultSpec& spec) {
    const char* dir = spec.tx && spec.rx ? "в обе стороны" : spec.rx ? "на приёме" : "на отправке";
    char buf[160];
    std::snprintf(buf, sizeof buf, "потеря %g, искажение %g на байт %s, seed %llu",
                  spec.drop, spec.flip, dir, (unsigned long long)spec.seed);
    return buf;
}

FaultyTransport::FaultyTransport(std::unique_ptr<ITransport> inner, const FaultSpec& spec)
    : inner_(std::move(inner)), spec_(spec), rng_(spec.seed) {}

// Помехи применяются к копии данных. Если нижний уровень принял только часть искажённых байт,
// вызывающему сообщаем, сколько *исходных* байт обработано, — остальное он передаст повторно
// (и судьба этих байт будет разыграна заново). За вызов обрабатывается не больше BLOCK байт,
// чтобы при частичной записи не перебирать весь большой буфер снова и снова.
ssize_t FaultyTransport::write(const void* buf, size_t n) {
    if (!spec_.tx) return inner_->write(buf, n);
    constexpr size_t BLOCK = 16 * 1024;
    n = std::min(n, BLOCK);
    const uint8_t* src = static_cast<const uint8_t*>(buf);
    out_.clear();
    origin_.clear();
    flipped_at_.clear();
    for (size_t i = 0; i < n; ++i) {
        if (chance(spec_.drop)) continue;
        uint8_t b = src[i];
        bool flip = chance(spec_.flip);
        if (flip) b = flip_bit(b);
        out_.push_back(b);
        origin_.push_back(i);
        flipped_at_.push_back(flip);
    }
    size_t k = 0;   // сколько байт после помех ушло в линию
    if (!out_.empty()) {
        ssize_t w = inner_->write(out_.data(), out_.size());
        if (w <= 0) return w;
        k = static_cast<size_t>(w);
    }
    size_t consumed = k == out_.size() ? n : origin_[k];
    dropped_ += consumed - k;
    flipped_ += static_cast<uint64_t>(std::count(flipped_at_.begin(), flipped_at_.begin() + static_cast<std::ptrdiff_t>(k), true));
    return static_cast<ssize_t>(consumed);
}

// Принятые байты искажаются на месте, потерянные выбрасываются. Если потерялось всё прочитанное,
// возвращаем EAGAIN, а не 0: ноль означал бы закрытие соединения.
ssize_t FaultyTransport::read(void* buf, size_t n) {
    ssize_t r = inner_->read(buf, n);
    if (!spec_.rx || r <= 0) return r;
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t kept = 0;
    for (size_t i = 0; i < static_cast<size_t>(r); ++i) {
        if (chance(spec_.drop)) { ++dropped_; continue; }
        uint8_t b = p[i];
        if (chance(spec_.flip)) { b = flip_bit(b); ++flipped_; }
        p[kept++] = b;
    }
    if (kept == 0) { errno = EAGAIN; return -1; }
    return static_cast<ssize_t>(kept);
}

std::unique_ptr<ITransport> FaultyConnector::connect(std::string& err) {
    auto t = inner_.connect(err);
    if (!t) return nullptr;
    FaultSpec s = spec_;
    spec_.seed = spec_.seed * 6364136223846793005ull + 1442695040888963407ull;   // новое соединение — новые помехи
    return std::make_unique<FaultyTransport>(std::move(t), s);
}

}  // namespace filexfer
