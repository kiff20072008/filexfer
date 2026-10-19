// Юнит-тесты протокола и fault-injection транспорта.
// Сторонние фреймворки не используются (условие задания) — ниже минимальный свой.
// Код протокола не зависит от сокетов: соединение проверяется поверх транспорта в памяти.

#include "fault_transport.hpp"
#include "protocol.hpp"
#include "util.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <random>
#include <string>
#include <vector>

using namespace filexfer;

// ---------------------------------------------------------------------------
// Мини-фреймворк
// ---------------------------------------------------------------------------
namespace {

int g_failures = 0;
std::string g_note;   // доп. информация, которую тест выводит после статуса

struct TestCase {
    const char* name;
    std::function<void()> fn;
};

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

}  // namespace

#define TEST(name)                                         \
    static void test_##name();                             \
    static const Registrar registrar_##name(#name, test_##name); \
    static void test_##name()

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            ++g_failures;                                                              \
            std::fprintf(stderr, "    %s:%d: CHECK(%s) не выполнено\n", __FILE__, __LINE__, #cond); \
        }                                                                              \
    } while (0)

// ---------------------------------------------------------------------------
// Транспорт в памяти: два однонаправленных канала
// ---------------------------------------------------------------------------
namespace {

struct Channel {
    std::deque<uint8_t> bytes;
    bool closed = false;
};

class MemTransport : public ITransport {
public:
    MemTransport(std::shared_ptr<Channel> in, std::shared_ptr<Channel> out, size_t max_write = SIZE_MAX)
        : in_(std::move(in)), out_(std::move(out)), max_write_(max_write) {}
    int fd() const override { return -1; }
    ssize_t read(void* buf, size_t n) override {
        if (in_->bytes.empty()) {
            if (in_->closed) return 0;
            errno = EAGAIN;
            return -1;
        }
        size_t k = std::min(n, in_->bytes.size());
        std::copy_n(in_->bytes.begin(), k, static_cast<uint8_t*>(buf));
        in_->bytes.erase(in_->bytes.begin(), in_->bytes.begin() + static_cast<std::ptrdiff_t>(k));
        return static_cast<ssize_t>(k);
    }
    ssize_t write(const void* buf, size_t n) override {   // max_write имитирует частичную запись
        size_t k = std::min(n, max_write_);
        const uint8_t* p = static_cast<const uint8_t*>(buf);
        out_->bytes.insert(out_->bytes.end(), p, p + k);
        return static_cast<ssize_t>(k);
    }
    std::string peer() const override { return "mem"; }
private:
    std::shared_ptr<Channel> in_, out_;
    size_t max_write_;
};

// Однонаправленная линия: sender пишет в wire, receiver читает из wire
struct Link {
    std::shared_ptr<Channel> wire = std::make_shared<Channel>();
    std::shared_ptr<Channel> back = std::make_shared<Channel>();

    std::unique_ptr<ITransport> sender_transport(size_t max_write = SIZE_MAX) {
        return std::make_unique<MemTransport>(back, wire, max_write);
    }
    std::unique_ptr<Connection> receiver() {
        return std::make_unique<Connection>(std::make_unique<MemTransport>(wire, back));
    }
};

std::vector<uint8_t> bytes_of(const std::string& s) { return {s.begin(), s.end()}; }

std::vector<uint8_t> random_bytes(size_t n, uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<uint8_t> v(n);
    for (auto& b : v) b = static_cast<uint8_t>(rng());
    return v;
}

// Полезная нагрузка, по которой можно проверить целостность: номер(4) + байты, вычисляемые из номера
std::vector<uint8_t> numbered_payload(uint32_t index, size_t len) {
    Writer w;
    w.u32(index);
    std::vector<uint8_t> v = w.data();
    for (size_t j = 0; j < len; ++j) v.push_back(static_cast<uint8_t>(index * 31 + j * 7));
    return v;
}

bool payload_intact(const std::vector<uint8_t>& p, uint32_t& index) {
    Reader r(p.data(), p.size());
    index = r.u32();
    if (!r.ok()) return false;
    for (size_t j = 0; j < r.left(); ++j)
        if (r.rest()[j] != static_cast<uint8_t>(index * 31 + j * 7)) return false;
    return true;
}

// Читает из линии всё, что в ней есть, и возвращает разобранные кадры
std::vector<Frame> drain(Connection& c, Channel& wire) {
    std::vector<Frame> frames;
    Frame f;
    do {
        c.pump_read();
        while (c.next(f) == Connection::Next::Frame) frames.push_back(f);
    } while (!wire.bytes.empty());
    return frames;
}

}  // namespace

// ---------------------------------------------------------------------------
// Утилиты
// ---------------------------------------------------------------------------
TEST(crc32_known_vectors) {
    CHECK(crc32("", 0) == 0);
    CHECK(crc32("123456789", 9) == 0xCBF43926u);
    auto data = random_bytes(10000, 1);
    uint32_t part = crc32(data.data(), 4000);
    CHECK(crc32(data.data() + 4000, 6000, part) == crc32(data.data(), data.size()));
}

TEST(fnv1a64_known_vectors) {
    CHECK(fnv1a64("", 0) == 0xCBF29CE484222325ull);
    CHECK(fnv1a64("a", 1) == 0xAF63DC4C8601EC8Cull);
}

TEST(printable_replaces_control_chars) {
    CHECK(printable("файл.bin") == "файл.bin");
    CHECK(printable("a\nb\x01\x7f") == "a?b??");
}

TEST(writer_reader_roundtrip) {
    Writer w;
    w.u8(0xAB).u16(0x1234).u32(0xDEADBEEF).u64(0x0102030405060708ull).str("привет");
    const auto& d = w.data();
    CHECK(d.size() == 1 + 2 + 4 + 8 + 2 + std::strlen("привет"));
    CHECK(d[3] == 0xDE);   // big-endian

    Reader r(d.data(), d.size());
    CHECK(r.u8() == 0xAB);
    CHECK(r.u16() == 0x1234);
    CHECK(r.u32() == 0xDEADBEEF);
    CHECK(r.u64() == 0x0102030405060708ull);
    CHECK(r.str() == "привет");
    CHECK(r.ok());
    CHECK(r.left() == 0);
}

TEST(reader_detects_truncation) {
    Writer w;
    w.u64(42).str("hello");
    for (size_t cut = 0; cut < w.data().size(); ++cut) {
        Reader r(w.data().data(), cut);
        r.u64();
        r.str();
        CHECK(!r.ok());
    }
}

// ---------------------------------------------------------------------------
// Кадрирование
// ---------------------------------------------------------------------------
TEST(frames_roundtrip) {
    Link link;
    Connection tx(link.sender_transport());
    auto big = random_bytes(32 * 1024, 2);
    tx.queue(MSG_START, bytes_of("abc"));
    tx.queue(MSG_DATA, big);
    tx.queue(MSG_END, std::vector<uint8_t>{});
    CHECK(tx.pump_write());
    CHECK(!tx.has_output());

    auto rx = link.receiver();
    auto frames = drain(*rx, *link.wire);
    CHECK(frames.size() == 3);
    if (frames.size() == 3) {
        CHECK(frames[0].type == MSG_START && frames[0].payload == bytes_of("abc"));
        CHECK(frames[1].type == MSG_DATA && frames[1].payload == big);
        CHECK(frames[2].type == MSG_END && frames[2].payload.empty());
    }
    CHECK(!rx->take_corrupt());
}

TEST(two_part_frame_equals_single_part) {
    Link a, b;
    Connection ta(a.sender_transport()), tb(b.sender_transport());
    auto head = bytes_of("HEAD"), body = random_bytes(1000, 3);
    std::vector<uint8_t> whole = head;
    whole.insert(whole.end(), body.begin(), body.end());
    ta.queue(MSG_DATA, head.data(), head.size(), body.data(), body.size());
    tb.queue(MSG_DATA, whole);
    ta.pump_write();
    tb.pump_write();
    CHECK(a.wire->bytes == b.wire->bytes);
}

TEST(partial_writes_are_completed) {
    Link link;
    Connection tx(link.sender_transport(7));   // транспорт принимает максимум 7 байт за вызов
    auto payload = random_bytes(5000, 4);
    tx.queue(MSG_DATA, payload);
    CHECK(tx.pump_write());
    CHECK(!tx.has_output());
    auto rx = link.receiver();
    auto frames = drain(*rx, *link.wire);
    CHECK(frames.size() == 1 && frames[0].payload == payload);
}

TEST(frame_arriving_byte_by_byte) {
    Link link;
    Connection tx(link.sender_transport());
    auto payload = random_bytes(300, 5);
    tx.queue(MSG_DATA, payload);
    tx.pump_write();
    std::deque<uint8_t> all;
    all.swap(link.wire->bytes);

    auto rx = link.receiver();
    Frame f;
    size_t got = 0;
    while (!all.empty()) {
        link.wire->bytes.push_back(all.front());
        all.pop_front();
        rx->pump_read();
        if (rx->next(f) == Connection::Next::Frame) {
            ++got;
            CHECK(all.empty());           // кадр готов только после последнего байта
            CHECK(f.payload == payload);
        }
    }
    CHECK(got == 1);
    CHECK(!rx->take_corrupt());
}

TEST(resync_after_garbage) {
    Link link;
    Connection tx(link.sender_transport());
    auto garbage = random_bytes(1000, 6);
    for (size_t i = 0; i + 1 < garbage.size(); i += 97) { garbage[i] = 0xA5; garbage[i + 1] = 0x5A; }   // ложные magic

    link.wire->bytes.insert(link.wire->bytes.end(), garbage.begin(), garbage.end());
    tx.queue(MSG_ACK, bytes_of("first"));
    tx.pump_write();
    link.wire->bytes.insert(link.wire->bytes.end(), garbage.begin(), garbage.begin() + 333);
    tx.queue(MSG_NACK, bytes_of("second"));
    tx.pump_write();

    auto rx = link.receiver();
    auto frames = drain(*rx, *link.wire);
    CHECK(frames.size() == 2);
    if (frames.size() == 2) {
        CHECK(frames[0].type == MSG_ACK && frames[0].payload == bytes_of("first"));
        CHECK(frames[1].type == MSG_NACK && frames[1].payload == bytes_of("second"));
    }
    CHECK(rx->take_corrupt());
    CHECK(!rx->take_corrupt());   // флаг сбрасывается
}

TEST(corrupted_payload_frame_is_dropped) {
    Link link;
    Connection tx(link.sender_transport());
    for (uint32_t i = 0; i < 3; ++i) tx.queue(MSG_DATA, numbered_payload(i, 100));
    tx.pump_write();
    size_t frame_size = HEADER + 4 + 100;
    link.wire->bytes[frame_size + HEADER + 50] ^= 0x10;   // байт данных второго кадра

    auto rx = link.receiver();
    auto frames = drain(*rx, *link.wire);
    uint32_t i0 = 99, i1 = 99;
    CHECK(frames.size() == 2);
    if (frames.size() == 2) {
        CHECK(payload_intact(frames[0].payload, i0) && i0 == 0);
        CHECK(payload_intact(frames[1].payload, i1) && i1 == 2);
    }
    CHECK(rx->take_corrupt());
}

TEST(corrupted_header_frame_is_skipped) {
    Link link;
    Connection tx(link.sender_transport());
    for (uint32_t i = 0; i < 3; ++i) tx.queue(MSG_DATA, numbered_payload(i, 100));
    tx.pump_write();
    size_t frame_size = HEADER + 4 + 100;
    link.wire->bytes[frame_size + 6] ^= 0x01;   // поле len второго кадра

    auto rx = link.receiver();
    auto frames = drain(*rx, *link.wire);
    uint32_t idx = 99;
    CHECK(frames.size() == 2);
    if (frames.size() == 2) CHECK(payload_intact(frames[1].payload, idx) && idx == 2);
    CHECK(rx->take_corrupt());
}

TEST(oversized_length_is_rejected) {
    // Заголовок с корректной CRC, но len > MAX_PAYLOAD: приёмник не должен ждать мегабайт данных
    Writer h;
    h.u16(MAGIC).u8(MSG_DATA).u8(VERSION).u32(static_cast<uint32_t>(MAX_PAYLOAD + 1)).u32(0);
    std::vector<uint8_t> hdr = h.data();
    Writer hc;
    hc.u32(crc32(hdr.data(), hdr.size()));
    hdr.insert(hdr.end(), hc.data().begin(), hc.data().end());

    Link link;
    link.wire->bytes.insert(link.wire->bytes.end(), hdr.begin(), hdr.end());
    Connection tx(link.sender_transport());
    tx.queue(MSG_ACK, bytes_of("after"));
    tx.pump_write();

    auto rx = link.receiver();
    auto frames = drain(*rx, *link.wire);
    CHECK(frames.size() == 1 && frames[0].payload == bytes_of("after"));
}

TEST(wrong_version_is_ignored) {
    Link link;
    Connection tx(link.sender_transport());
    tx.queue(MSG_ACK, bytes_of("x"));
    tx.pump_write();
    link.wire->bytes[3] = VERSION + 1;
    auto rx = link.receiver();
    CHECK(drain(*rx, *link.wire).empty());
}

TEST(eof_is_reported) {
    Link link;
    link.wire->closed = true;
    auto rx = link.receiver();
    CHECK(!rx->pump_read());
}

// ---------------------------------------------------------------------------
// Fault-injection транспорт
// ---------------------------------------------------------------------------
TEST(fault_spec_parsing) {
    FaultSpec s;
    std::string err;
    CHECK(parse_fault_spec("drop=0.01,flip=0.002,seed=5", s, err));
    CHECK(s.drop == 0.01 && s.flip == 0.002 && s.seed == 5);
    CHECK(parse_fault_spec("flip=1", s, err) && s.flip == 1.0 && s.drop == 0.0);
    CHECK(!parse_fault_spec("", s, err));
    CHECK(!parse_fault_spec("drop=2", s, err));
    CHECK(!parse_fault_spec("drop=-0.1", s, err));
    CHECK(!parse_fault_spec("drop=abc", s, err));
    CHECK(!parse_fault_spec("drop=", s, err));
    CHECK(!parse_fault_spec("loss=0.1", s, err));
    CHECK(!parse_fault_spec("seed=1x", s, err));

    CHECK(parse_fault_spec("drop=0.1", s, err) && s.tx && !s.rx);   // по умолчанию — только отправка
    CHECK(parse_fault_spec("drop=0.1,dir=rx", s, err) && !s.tx && s.rx);
    CHECK(parse_fault_spec("dir=both,flip=0.1", s, err) && s.tx && s.rx);
    CHECK(!parse_fault_spec("dir=up", s, err));
}

TEST(fault_transport_without_faults_is_transparent) {
    Link link;
    FaultSpec none;
    Connection tx(std::make_unique<FaultyTransport>(link.sender_transport(5), none));
    for (uint32_t i = 0; i < 20; ++i) tx.queue(MSG_DATA, numbered_payload(i, 333));
    CHECK(tx.pump_write());
    CHECK(!tx.has_output());
    auto rx = link.receiver();
    auto frames = drain(*rx, *link.wire);
    CHECK(frames.size() == 20);
    CHECK(!rx->take_corrupt());
}

TEST(fault_transport_partial_write_accounting) {
    // Частичная запись нижнего уровня + потеря байтов: Connection должен отдать всё, ничего не продублировав
    Link link;
    FaultSpec spec;
    spec.drop = 0.05;
    spec.seed = 11;
    auto ft = std::make_unique<FaultyTransport>(link.sender_transport(3), spec);
    FaultyTransport* raw = ft.get();
    Connection tx(std::move(ft));
    std::vector<uint8_t> payload(10000, 0x42);
    tx.queue(MSG_DATA, payload);
    CHECK(tx.pump_write());
    CHECK(!tx.has_output());
    size_t sent = HEADER + payload.size();
    CHECK(raw->dropped() > 0);
    CHECK(link.wire->bytes.size() + raw->dropped() == sent);
    CHECK(link.wire->bytes.size() < sent);
}

TEST(fault_transport_is_deterministic) {
    auto run = [](uint64_t seed) {
        Link link;
        FaultSpec spec;
        spec.drop = 0.01;
        spec.flip = 0.01;
        spec.seed = seed;
        FaultyTransport t(link.sender_transport(), spec);
        auto data = random_bytes(5000, 7);
        t.write(data.data(), data.size());
        return std::vector<uint8_t>(link.wire->bytes.begin(), link.wire->bytes.end());
    };
    CHECK(run(1) == run(1));
    CHECK(run(1) != run(2));
}

TEST(fault_transport_rx_only_leaves_tx_intact) {
    Link link;
    FaultSpec spec;
    spec.drop = 1.0;   // на приёме теряется всё
    spec.tx = false;
    spec.rx = true;
    FaultyTransport t(link.sender_transport(), spec);
    auto data = random_bytes(1000, 8);
    CHECK(t.write(data.data(), data.size()) == static_cast<ssize_t>(data.size()));
    CHECK(std::vector<uint8_t>(link.wire->bytes.begin(), link.wire->bytes.end()) == data);
}

TEST(fault_transport_rx_all_dropped_is_eagain_not_eof) {
    // Если на приёме потерялось всё прочитанное, это «данных нет», а не закрытие соединения
    Link link;
    auto data = random_bytes(100, 9);
    link.wire->bytes.assign(data.begin(), data.end());
    FaultSpec spec;
    spec.drop = 1.0;
    spec.tx = false;
    spec.rx = true;
    auto ft = std::make_unique<FaultyTransport>(std::make_unique<MemTransport>(link.wire, link.back), spec);
    uint8_t buf[256];
    errno = 0;
    CHECK(ft->read(buf, sizeof buf) == -1 && errno == EAGAIN);
    CHECK(ft->dropped() == data.size());
    Connection rx(std::move(ft));
    CHECK(rx.pump_read());   // соединение живо
}

TEST(noisy_receive_never_delivers_corrupt_frames) {
    Link link;
    Connection tx(link.sender_transport());
    const uint32_t total = 300;
    for (uint32_t i = 0; i < total; ++i) tx.queue(MSG_DATA, numbered_payload(i, 200));
    tx.pump_write();

    FaultSpec spec;
    spec.drop = 0.002;
    spec.flip = 0.002;
    spec.seed = 3;
    spec.tx = false;
    spec.rx = true;
    Connection rx(std::make_unique<FaultyTransport>(std::make_unique<MemTransport>(link.wire, link.back), spec));
    auto frames = drain(rx, *link.wire);
    bool all_intact = true;
    for (auto& f : frames) {
        uint32_t idx = 0;
        if (!payload_intact(f.payload, idx) || idx >= total) all_intact = false;
    }
    CHECK(all_intact);
    CHECK(frames.size() > total / 5 && frames.size() < total);
    CHECK(rx.take_corrupt());
}

TEST(noisy_channel_never_delivers_corrupt_frames) {
    // Главное свойство протокола: при потере и искажении байтов кадры теряются, но не портятся,
    // а поток после сбоя восстанавливается.
    Link link;
    FaultSpec spec;
    spec.drop = 0.002;
    spec.flip = 0.002;
    spec.seed = 7;
    auto ft = std::make_unique<FaultyTransport>(link.sender_transport(), spec);
    FaultyTransport* raw = ft.get();
    Connection tx(std::move(ft));
    const uint32_t total = 500;
    for (uint32_t i = 0; i < total; ++i) {
        tx.queue(MSG_DATA, numbered_payload(i, 200));
        tx.pump_write();
    }
    CHECK(raw->dropped() > 0 && raw->flipped() > 0);

    auto rx = link.receiver();
    auto frames = drain(*rx, *link.wire);
    bool all_intact = true, increasing = true;
    uint32_t prev = 0;
    for (size_t k = 0; k < frames.size(); ++k) {
        uint32_t idx = 0;
        if (!payload_intact(frames[k].payload, idx) || idx >= total) all_intact = false;
        if (k && idx <= prev) increasing = false;
        prev = idx;
    }
    CHECK(all_intact);
    CHECK(increasing);
    CHECK(frames.size() > total / 5);   // ~40% кадров по 220 байт проходят без повреждений
    CHECK(frames.size() < total);
    CHECK(rx->take_corrupt());
    char note[160];
    std::snprintf(note, sizeof note, "доставлено %zu из %u кадров, потеряно байт: %llu, искажено: %llu", frames.size(), total,
                  (unsigned long long)raw->dropped(), (unsigned long long)raw->flipped());
    g_note = note;
}

// ---------------------------------------------------------------------------
// Параметры канала
// ---------------------------------------------------------------------------
TEST(tcp_link_params_defaults) {
    LinkParams p = tcp_link_params();
    CHECK(p.max_chunk == 32 * 1024 && p.min_chunk == 512 && p.window_bytes == 512 * 1024);
    CHECK(p.retransmit_ms == 3000 && p.peer_dead_ms == 15000);
}

TEST(serial_link_params_are_consistent) {
    uint64_t prev_window = 0;
    for (unsigned baud : {1200u, 9600u, 57600u, 115200u, 921600u, 4000000u}) {
        LinkParams p = serial_link_params(baud);
        uint64_t bytes_per_sec = baud / 10;
        int64_t window_ms = static_cast<int64_t>(p.window_bytes * 1000 / bytes_per_sec);
        CHECK(p.min_chunk <= p.max_chunk);
        CHECK(p.max_chunk + 8 <= MAX_PAYLOAD);
        CHECK(p.window_bytes >= p.max_chunk);
        CHECK(p.retransmit_ms > window_ms);              // окно успевает уйти до повтора
        CHECK(p.peer_dead_ms > p.retransmit_ms);
        CHECK(p.idle_ms > p.peer_dead_ms);               // сервер не закрывает соединение, пока клиент ждёт
        CHECK(p.resume_ttl_ms > p.idle_ms);
        CHECK(p.window_bytes >= prev_window);            // быстрее канал — больше окно
        prev_window = p.window_bytes;
    }
    CHECK(serial_link_params(115200).window_bytes < tcp_link_params().window_bytes);
}

// ---------------------------------------------------------------------------
int main() {
    int failed_tests = 0;
    for (auto& t : registry()) {
        int before = g_failures;
        std::printf("  %-50s ", t.name);
        std::fflush(stdout);
        t.fn();
        bool ok = g_failures == before;
        std::printf("%s\n", ok ? "OK" : "FAIL");
        if (!g_note.empty()) { std::printf("      %s\n", g_note.c_str()); g_note.clear(); }
        if (!ok) ++failed_tests;
    }
    std::printf("\nЮнит-тесты: %zu, провалено: %d\n", registry().size(), failed_tests);
    return failed_tests ? 1 : 0;
}
