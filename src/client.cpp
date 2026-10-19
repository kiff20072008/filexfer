#include "client.hpp"

#include "log.hpp"
#include "protocol.hpp"
#include "unique_fd.hpp"
#include "util.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <vector>

namespace filexfer {

namespace {

// Размер блока, окно и таймауты зависят от канала и приходят из IConnector::params()
constexpr int64_t CLIENT_RETRY_DELAY = 2'000;
constexpr int     CLIENT_MAX_FAILS   = 10;          // подряд неудачных попыток без прогресса
constexpr int     POLL_INTERVAL_MS   = 200;

// Номер сигнала (SIGINT/SIGTERM), если пользователь прервал клиента. Обработчик без SA_RESTART,
// поэтому poll() сразу возвращается с EINTR и цикл замечает флаг.
volatile std::sig_atomic_t g_interrupted = 0;

void on_signal(int sig) { g_interrupted = sig; }

void install_signal_handlers() {
    struct sigaction sa {};
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
}

// Пауза, которую прерывает сигнал
void interruptible_sleep(int64_t ms) { poll(nullptr, 0, static_cast<int>(ms)); }

class Sender {
public:
    Sender(IConnector& c, std::string path)
        : conn_(c), lp_(c.params()), path_(std::move(path)), chunk_(lp_.max_chunk) {}

    int run() {
        if (!open_file()) return g_interrupted ? interrupted() : 1;

        int fails = 0;
        for (;;) {
            progress_made_ = false;
            Outcome o = session();
            if (o == Outcome::Done) return 0;
            if (o == Outcome::Fatal) return 1;
            if (o == Outcome::Interrupted) return interrupted();
            if (progress_made_) fails = 0;
            if (++fails > CLIENT_MAX_FAILS) {
                log_msg("клиент", "Передача не удалась: нет связи с сервером (%d попыток подряд)", CLIENT_MAX_FAILS);
                return 1;
            }
            log_msg("клиент", "Повторное подключение через %lld с (попытка %d из %d)…", (long long)(CLIENT_RETRY_DELAY / 1000), fails, CLIENT_MAX_FAILS);
            interruptible_sleep(CLIENT_RETRY_DELAY);
            if (g_interrupted) return interrupted();
        }
    }

private:
    enum class Outcome { Continue, Done, Retry, Fatal, Interrupted };

    // Ctrl+C / SIGTERM: сервер сохранит принятую часть, и повторный запуск продолжит с неё
    int interrupted() {
        if (announced_)
            log_msg("клиент", "Передача прервана: сервер подтвердил %llu из %llu байт. "
                              "Повторный запуск с тем же файлом в течение %lld с продолжит с этого места",
                    (unsigned long long)acked_, (unsigned long long)size_, (long long)(lp_.resume_ttl_ms / 1000));
        else
            log_msg("клиент", "Прервано до начала передачи");
        return 128 + static_cast<int>(g_interrupted);
    }

    bool open_file() {
        fd_.reset(::open(path_.c_str(), O_RDONLY | O_CLOEXEC));
        struct stat st;
        if (!fd_ || fstat(fd_.get(), &st) < 0) {
            log_msg("клиент", "Не удалось открыть файл «%s»: %s", path_.c_str(), strerror(errno));
            return false;
        }
        if (!S_ISREG(st.st_mode)) {
            log_msg("клиент", "«%s» — не обычный файл", path_.c_str());
            return false;
        }
        size_ = static_cast<uint64_t>(st.st_size);
        size_t sl = path_.rfind('/');
        name_ = sl == std::string::npos ? path_ : path_.substr(sl + 1);

        if (size_ >= (64u << 20))
            log_msg("клиент", "Подсчёт контрольной суммы «%s» (%llu байт)…", name_.c_str(), (unsigned long long)size_);
        std::vector<uint8_t> buf(1 << 20);       // CRC всего файла для итоговой проверки
        uint64_t off = 0;
        crc_ = 0;
        while (off < size_) {
            ssize_t n = pread(fd_.get(), buf.data(), std::min<uint64_t>(buf.size(), size_ - off), static_cast<off_t>(off));
            if (g_interrupted) return false;
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { log_msg("клиент", "Ошибка чтения файла: %s", n < 0 ? strerror(errno) : "файл укоротился"); return false; }
            crc_ = crc32(buf.data(), static_cast<size_t>(n), crc_);
            off += static_cast<uint64_t>(n);
        }

        // id передачи зависит только от файла: если процесс клиента прервать и запустить снова,
        // сервер узнает передачу и продолжит её с места обрыва (пока не истёк LinkParams::resume_ttl_ms).
        uint64_t h = fnv1a64(name_.data(), name_.size());
        const uint64_t meta[] = {size_, uint64_t(st.st_mtim.tv_sec), uint64_t(st.st_mtim.tv_nsec), crc_};
        id_ = fnv1a64(meta, sizeof meta, h);
        return true;
    }

    void show_progress(bool force = false) {
        int64_t now = now_ms();
        if (!tty_ || (!force && now - last_progress_print_ < 150)) return;
        last_progress_print_ = now;
        log_progress("[клиент] отправлено %3d%% (%llu / %llu байт)", size_ ? int(acked_ * 100 / size_) : 100,
                     (unsigned long long)acked_, (unsigned long long)size_);
    }

    // Одно подключение: START -> DATA… -> END -> DONE
    Outcome session() {
        std::string err;
        auto t = conn_.connect(err);
        if (g_interrupted) return Outcome::Interrupted;
        if (!t) {
            if (!connected_once_) {   // сервера нет с самого начала — ждать бессмысленно, сообщаем сразу
                log_msg("клиент", "Сервер %s недоступен: %s. Запущен ли он (filexfer -s)?", conn_.describe().c_str(), err.c_str());
                return Outcome::Fatal;
            }
            log_msg("клиент", "Не удалось подключиться к %s: %s", conn_.describe().c_str(), err.c_str());
            return Outcome::Retry;
        }
        connected_once_ = true;
        Connection c(std::move(t));
        {
            Writer w; w.u64(id_).u64(size_).u8(announced_ ? START_RECONNECT : 0).str(name_);
            c.queue(MSG_START, w.data());
        }
        bool started = false, end_sent = false;
        int64_t last_rx = now_ms(), last_progress = last_rx, end_time = 0;
        std::vector<uint8_t> chunk(lp_.max_chunk);
        sent_ = acked_ = 0;

        for (;;) {
            if (g_interrupted) return Outcome::Interrupted;
            int64_t now = now_ms();
            if (started) {
                while (!c.has_output() && sent_ < size_ && sent_ - acked_ < lp_.window_bytes) {
                    size_t len = static_cast<size_t>(std::min<uint64_t>(chunk_, size_ - sent_));
                    ssize_t n = pread(fd_.get(), chunk.data(), len, static_cast<off_t>(sent_));
                    if (n != static_cast<ssize_t>(len)) {
                        log_msg("клиент", "Ошибка чтения файла (изменился во время передачи?)");
                        return Outcome::Fatal;
                    }
                    Writer h; h.u64(sent_);
                    c.queue(MSG_DATA, h.data().data(), 8, chunk.data(), len);
                    sent_ += len;
                    if (!c.pump_write()) return Outcome::Retry;
                }
                if (!end_sent && acked_ == size_ && !c.has_output()) {
                    Writer w; w.u64(size_).u32(crc_);
                    c.queue(MSG_END, w.data());
                    end_sent = true;
                    end_time = now;
                }
            }
            if (!c.pump_write()) return Outcome::Retry;

            pollfd pfd{c.fd(), static_cast<short>(POLLIN | (c.has_output() ? POLLOUT : 0)), 0};
            int r = poll(&pfd, 1, POLL_INTERVAL_MS);
            if (r < 0 && errno != EINTR) { log_msg("клиент", "poll: %s", strerror(errno)); return Outcome::Fatal; }
            now = now_ms();

            if (r > 0) {
                bool alive = true;
                if (pfd.revents & (POLLIN | POLLHUP | POLLERR)) alive = c.pump_read();
                Frame f;
                while (c.next(f) == Connection::Next::Frame) {
                    last_rx = now;
                    Outcome o = on_frame(f, started, end_sent, last_progress);
                    if (o != Outcome::Continue) return o;
                }
                if (!alive) { log_msg("клиент", "Соединение разорвано"); return Outcome::Retry; }
            }

            if (now - last_rx > lp_.peer_dead_ms) { log_msg("клиент", "Сервер не отвечает"); return Outcome::Retry; }
            if (started && acked_ < sent_ && now - last_progress > lp_.retransmit_ms) {   // потеря данных/ACK
                chunk_ = std::max(lp_.min_chunk, chunk_ / 2);
                sent_ = acked_;
                last_progress = now;
                end_sent = false;
                ++retransmits_;
            }
            if (end_sent && now - end_time > lp_.retransmit_ms) end_sent = false;           // повторить END
            if (started) show_progress();
        }
    }

    Outcome on_frame(const Frame& f, bool& started, bool& end_sent, int64_t& last_progress) {
        Reader r(f.payload.data(), f.payload.size());
        switch (f.type) {
            case MSG_START_ACK: {
                uint64_t id = r.u64(), off = r.u64();
                uint8_t st = r.u8();
                std::string name = r.str();
                if (!r.ok()) return Outcome::Continue;
                if (st == ST_ALREADY_DONE) { finish(name); return Outcome::Done; }
                if (st != ST_OK || off > size_) {
                    log_msg("клиент", "Сервер отклонил передачу: %s", status_text(st));
                    return Outcome::Fatal;
                }
                id_ = id;            // сервер мог выдать другой id (тот же файл уже передаётся параллельно)
                started = true;
                sent_ = acked_ = off;
                last_progress = now_ms();
                if (!announced_) {
                    announced_ = true;
                    t_start_ = now_ms();
                    log_msg("клиент", "Начало передачи: «%s», %llu байт -> %s", name_.c_str(), (unsigned long long)size_, conn_.describe().c_str());
                    if (off > 0)
                        log_msg("клиент", "Сервер уже принял %llu байт от прерванного запуска — продолжаем с этого места", (unsigned long long)off);
                } else {
                    log_msg("клиент", "Передача возобновлена с позиции %llu из %llu байт", (unsigned long long)off, (unsigned long long)size_);
                }
                return Outcome::Continue;
            }
            case MSG_ACK: {
                uint64_t o = r.u64();
                if (r.ok() && o > acked_ && o <= size_) {
                    acked_ = o;
                    chunk_ = std::min(lp_.max_chunk, chunk_ + lp_.min_chunk);   // канал в порядке — блок растёт
                    if (sent_ < acked_) sent_ = acked_;
                    last_progress = now_ms();
                    progress_made_ = true;
                }
                return Outcome::Continue;
            }
            case MSG_NACK: {                                  // сервер просит повторить с позиции o
                uint64_t o = r.u64();
                if (r.ok() && o >= acked_ && o <= sent_) {
                    chunk_ = std::max(lp_.min_chunk, chunk_ / 2);        // потери — уменьшаем блок вдвое
                    sent_ = o;
                    if (o > acked_) { acked_ = o; progress_made_ = true; }
                    last_progress = now_ms();
                    end_sent = false;
                    ++retransmits_;
                }
                return Outcome::Continue;
            }
            case MSG_DONE: {
                uint8_t st = r.u8();
                std::string name = r.str();
                if (!r.ok()) return Outcome::Continue;
                if (st == ST_OK) { finish(name); return Outcome::Done; }
                log_msg("клиент", "Передача завершилась ошибкой: %s", status_text(st));
                return Outcome::Fatal;
            }
            default: return Outcome::Continue;
        }
    }

    void finish(const std::string& saved) {
        acked_ = size_;
        show_progress(true);
        double sec = static_cast<double>(std::max<int64_t>(now_ms() - t_start_, 1)) / 1000.0;
        log_msg("клиент", "Передача завершена: «%s», %llu байт (%.1f с, повторов: %llu). Сервер сохранил файл как %s",
                name_.c_str(), (unsigned long long)size_, sec, (unsigned long long)retransmits_, saved.c_str());
    }

    IConnector& conn_;
    const LinkParams lp_;
    std::string path_, name_;
    size_t chunk_;                              // текущий размер блока (адаптивный)
    UniqueFd fd_;
    uint64_t size_ = 0, id_ = 0, sent_ = 0, acked_ = 0, retransmits_ = 0;
    uint32_t crc_ = 0;
    bool announced_ = false, progress_made_ = false, connected_once_ = false;
    int64_t t_start_ = 0, last_progress_print_ = 0;
    const bool tty_ = isatty(STDOUT_FILENO);
};

}  // namespace

int run_client(IConnector& connector, const std::string& path) {
    install_signal_handlers();
    Sender sender(connector, path);
    return sender.run();
}

}  // namespace filexfer
