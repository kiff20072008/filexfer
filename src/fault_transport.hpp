// Имитация ненадёжного канала (для тестов): декоратор над любым ITransport,
// который теряет и искажает отдельные байты — как COM-порт с помехами.
//
// В программе включается переменной окружения на стороне клиента:
//   FILEXFER_FAULTS="drop=0.0001,flip=0.0001,seed=42,dir=both" ./filexfer -c file
// dir=tx — помехи на отправке (данные к серверу), rx — на приёме (ответы сервера), both — в обе стороны.
#pragma once

#include "transport.hpp"

#include <cstdint>
#include <random>
#include <vector>

namespace filexfer {

struct FaultSpec {
    double drop = 0;      // вероятность потерять байт
    double flip = 0;      // вероятность инвертировать случайный бит байта
    uint64_t seed = 1;    // генератор детерминирован: одинаковый seed — одинаковые помехи
    bool tx = true;       // помехи при отправке
    bool rx = false;      // помехи при приёме
};

// Разбирает "drop=P,flip=P,seed=N,dir=tx|rx|both" (поля в любом порядке, любые можно опустить; dir по умолчанию tx).
bool parse_fault_spec(const std::string& text, FaultSpec& out, std::string& err);
std::string describe(const FaultSpec& spec);

class FaultyTransport : public ITransport {
public:
    FaultyTransport(std::unique_ptr<ITransport> inner, const FaultSpec& spec);
    int fd() const override { return inner_->fd(); }
    ssize_t read(void* buf, size_t n) override;
    ssize_t write(const void* buf, size_t n) override;
    std::string peer() const override { return inner_->peer(); }

    uint64_t dropped() const { return dropped_; }
    uint64_t flipped() const { return flipped_; }

private:
    bool chance(double p) { return p > 0 && uniform_(rng_) < p; }
    uint8_t flip_bit(uint8_t b) { return static_cast<uint8_t>(b ^ (1u << (rng_() % 8))); }

    std::unique_ptr<ITransport> inner_;
    FaultSpec spec_;
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uniform_{0.0, 1.0};
    std::vector<uint8_t> out_;     // байты после помех
    std::vector<size_t> origin_;   // origin_[i] — индекс исходного байта для out_[i]
    std::vector<bool> flipped_at_; // был ли искажён out_[i]
    uint64_t dropped_ = 0, flipped_ = 0;
};

// Оборачивает каждое соединение, которое создаёт внутренний коннектор.
class FaultyConnector : public IConnector {
public:
    FaultyConnector(IConnector& inner, const FaultSpec& spec) : inner_(inner), spec_(spec) {}
    std::unique_ptr<ITransport> connect(std::string& err) override;
    std::string describe() const override { return inner_.describe(); }
    LinkParams params() const override { return inner_.params(); }
private:
    IConnector& inner_;
    FaultSpec spec_;
};

}  // namespace filexfer
