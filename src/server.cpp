#include "server.hpp"

#include "log.hpp"
#include "protocol.hpp"
#include "unique_fd.hpp"
#include "util.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <random>
#include <stdexcept>
#include <vector>

namespace filexfer {

namespace {

constexpr int64_t SERVER_DONE_TTL = 10 * 60'000;   // помнить завершённые передачи (для повтора DONE)

class Server {
public:
    Server(IAcceptor& acc, std::string dir) : acc_(acc), lp_(acc.params()), dir_(std::move(dir)) {}
    ~Server() {
        for (auto& kv : sessions_)
            if (!kv.second->done) unlink(kv.second->part_path.c_str());
    }

    int run() {
        cleanup_stale_parts();

        sigset_t mask;
        sigemptyset(&mask);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTERM);
        sigprocmask(SIG_BLOCK, &mask, nullptr);
        sigfd_.reset(signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
        ep_.reset(epoll_create1(EPOLL_CLOEXEC));
        if (!sigfd_ || !ep_) throw std::runtime_error(std::string("epoll/signalfd: ") + strerror(errno));
        epoll_add(sigfd_.get());
        epoll_add(acc_.fd());

        log_msg("сервер", "Запущен (%s). Каталог для файлов: %s. Ожидание передачи…", acc_.describe().c_str(), dir_.c_str());

        for (bool stop = false; !stop;) {
            epoll_event evs[64];
            int n = epoll_wait(ep_.get(), evs, 64, next_timeout());   // без событий и таймеров — спим, CPU не тратится
            if (n < 0) {
                if (errno == EINTR) continue;
                throw std::runtime_error(std::string("epoll_wait: ") + strerror(errno));
            }
            for (int i = 0; i < n; ++i) {
                int fd = evs[i].data.fd;
                if (fd == sigfd_.get()) { stop = true; }
                else if (fd == acc_.fd()) { accept_all(); }
                else { handle_peer(fd, evs[i].events); }
            }
            expire_timers();
        }
        log_msg("сервер", "Остановка");
        return 0;
    }

private:
    struct Peer;
    struct Session {
        uint64_t id = 0, size = 0, received = 0;
        uint32_t crc = 0;
        UniqueFd file;
        std::string part_path, final_path, final_name, orig_name, peer_name;
        Peer* peer = nullptr;
        int64_t deadline = 0;   // 0 — без срока (клиент подключён)
        bool done = false, nack_sent = false, nack_pending = false;
        int64_t t_start = 0, nack_time = 0, nack_due = 0;
    };
    struct Peer {
        std::unique_ptr<Connection> conn;
        Session* sess = nullptr;
        int64_t last_act = 0;
        bool closing = false, want_out = false, ack_pending = false;
    };

    // ---- epoll ----
    void epoll_add(int fd) {
        epoll_event e{};
        e.events = EPOLLIN;
        e.data.fd = fd;
        if (epoll_ctl(ep_.get(), EPOLL_CTL_ADD, fd, &e) < 0) throw std::runtime_error(std::string("epoll_ctl: ") + strerror(errno));
    }
    void update_interest(Peer& p) {
        bool w = p.conn->has_output();
        if (w == p.want_out) return;
        epoll_event e{};
        e.events = EPOLLIN | (w ? static_cast<uint32_t>(EPOLLOUT) : 0u);
        e.data.fd = p.conn->fd();
        epoll_ctl(ep_.get(), EPOLL_CTL_MOD, e.data.fd, &e);
        p.want_out = w;
    }

    int next_timeout() const {
        int64_t nearest = INT64_MAX;
        for (auto& kv : peers_) nearest = std::min(nearest, kv.second->last_act + lp_.idle_ms);
        for (auto& kv : sessions_) {
            if (kv.second->deadline) nearest = std::min(nearest, kv.second->deadline);
            if (kv.second->nack_pending && kv.second->peer) nearest = std::min(nearest, kv.second->nack_due);
        }
        if (nearest == INT64_MAX) return -1;
        return static_cast<int>(std::min<int64_t>(std::max<int64_t>(nearest - now_ms(), 0) + 1, 3'600'000));
    }

    void expire_timers() {
        int64_t now = now_ms();
        std::vector<int> idle;
        for (auto& kv : peers_) if (now - kv.second->last_act >= lp_.idle_ms) idle.push_back(kv.first);
        for (int fd : idle) drop_peer(fd, "таймаут неактивности");

        std::vector<int> broken;                       // отложенные NACK
        for (auto& kv : sessions_) {
            Session* s = kv.second.get();
            if (s->nack_pending && s->peer && now >= s->nack_due && !s->peer->closing) {
                Peer& p = *s->peer;
                send_nack(p, s);
                if (!p.conn->pump_write()) broken.push_back(p.conn->fd()); else update_interest(p);
            }
        }
        for (int fd : broken) drop_peer(fd, "разрыв связи");

        std::vector<Session*> dead;
        for (auto& kv : sessions_) if (kv.second->deadline && now >= kv.second->deadline) dead.push_back(kv.second.get());
        for (Session* s : dead) {
            if (!s->done)
                log_msg("сервер", "Передача отменена: клиент %s не вернулся (принято %llu из %llu байт), временный файл удалён",
                        s->peer_name.c_str(), (unsigned long long)s->received, (unsigned long long)s->size);
            destroy_session(s, true);
        }
    }

    // ---- соединения ----
    void accept_all() {
        while (auto t = acc_.accept()) {
            auto p = std::make_unique<Peer>();
            int fd = t->fd();
            p->conn = std::make_unique<Connection>(std::move(t));
            p->last_act = now_ms();
            epoll_event e{};
            e.events = EPOLLIN;
            e.data.fd = fd;
            if (epoll_ctl(ep_.get(), EPOLL_CTL_ADD, fd, &e) < 0) {   // соединение отклоняем, сервер продолжает работу
                log_msg("сервер", "epoll_ctl: %s — соединение с %s закрыто", strerror(errno), p->conn->peer().c_str());
                continue;
            }
            peers_[fd] = std::move(p);
        }
    }

    void drop_peer(int fd, const char* why) {
        auto it = peers_.find(fd);
        if (it == peers_.end()) return;
        Peer& p = *it->second;
        if (Session* s = p.sess) {
            s->peer = nullptr;
            if (!s->done) {
                s->deadline = now_ms() + lp_.resume_ttl_ms;
                log_msg("сервер", "Соединение с %s прервано (%s): принято %llu из %llu байт, ожидание возобновления %lld с",
                        s->peer_name.c_str(), why, (unsigned long long)s->received, (unsigned long long)s->size,
                        (long long)(lp_.resume_ttl_ms / 1000));
            }
        }
        epoll_ctl(ep_.get(), EPOLL_CTL_DEL, fd, nullptr);
        peers_.erase(it);   // закрывает дескриптор
    }

    void handle_peer(int fd, uint32_t events) {
        auto it = peers_.find(fd);
        if (it == peers_.end()) return;
        Peer& p = *it->second;
        p.last_act = now_ms();
        bool alive = true;
        if (events & (EPOLLIN | EPOLLHUP | EPOLLERR)) {
            alive = p.conn->pump_read();
            process(p);
        }
        if (!p.conn->pump_write()) alive = false;
        if (!alive || (p.closing && !p.conn->has_output())) {
            drop_peer(fd, alive ? "закрыто сервером" : "разрыв связи");
            return;
        }
        update_interest(p);
    }

    // ---- обработка кадров ----
    void process(Peer& p) {
        Frame f;
        while (!p.closing && p.conn->next(f) == Connection::Next::Frame) {
            switch (f.type) {
                case MSG_START: on_start(p, f); break;
                case MSG_DATA:  on_data(p, f);  break;
                case MSG_END:   on_end(p, f);   break;
                default: break;
            }
        }
        Session* s = p.sess;
        if (p.conn->take_corrupt() && s && !s->done && !p.closing) nack_or_defer(p, s);
        if (p.ack_pending && s && !p.closing) {
            Writer w; w.u64(s->received);
            p.conn->queue(MSG_ACK, w.data());
            p.ack_pending = false;
        }
    }

    // NACK один раз на «дырку» (хвост уже отправленных клиентом блоков не должен вызывать лавину NACK),
    // но повторно — не раньше чем через lp_.renack_ms, если и повтор пришёл испорченным.
    // NACK заодно показывает клиенту, что сервер жив.
    // Подавленный NACK не теряется: он откладывается и уходит по таймеру (см. expire_timers), потому что после
    // потери последнего блока в полёте новых кадров, способных его вызвать, уже не будет.
    void send_nack(Peer& p, Session* s) {
        Writer w; w.u64(s->received);
        p.conn->queue(MSG_NACK, w.data());
        s->nack_sent = true;
        s->nack_pending = false;
        s->nack_time = now_ms();
    }
    void nack_or_defer(Peer& p, Session* s) {
        if (!s->nack_sent || now_ms() - s->nack_time >= lp_.renack_ms) {
            send_nack(p, s);
        } else {
            s->nack_pending = true;
            s->nack_due = s->nack_time + lp_.renack_ms;
        }
    }
    void send_done(Peer& p, uint8_t status, const std::string& name) {
        Writer w; w.u8(status).str(name);
        p.conn->queue(MSG_DONE, w.data());
    }
    void send_start_ack(Peer& p, uint64_t id, uint64_t off, uint8_t status, const std::string& name) {
        Writer w; w.u64(id).u64(off).u8(status).str(name);
        p.conn->queue(MSG_START_ACK, w.data());
    }

    bool create_files(Session& s, std::string& err) {
        time_t t = time(nullptr);
        tm tmv;
        localtime_r(&t, &tmv);
        char ts[32];
        strftime(ts, sizeof ts, "%Y%m%d_%H%M%S", &tmv);   // date_time
        for (int i = 0; i < 1000; ++i) {
            std::string name = ts;
            if (i) name += "_" + std::to_string(i);       // два файла в одну секунду
            name += ".hex";
            std::string fin = dir_ + "/" + name, part = fin + ".part";
            if (access(fin.c_str(), F_OK) == 0) continue;
            UniqueFd fd(open(part.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644));
            if (!fd) {
                if (errno == EEXIST) continue;
                err = strerror(errno);
                return false;
            }
            s.file = std::move(fd);
            s.part_path = part; s.final_path = fin; s.final_name = name;
            return true;
        }
        err = "нет свободного имени файла";
        return false;
    }

    void on_start(Peer& p, const Frame& f) {
        Reader r(f.payload.data(), f.payload.size());
        uint64_t id = r.u64(), size = r.u64();
        uint8_t flags = r.u8();
        std::string name = printable(r.str());
        if (!r.ok()) { p.closing = true; return; }
        bool reconnect = flags & START_RECONNECT;

        auto it = sessions_.find(id);
        if (it != sessions_.end()) {
            Session* s = it->second.get();
            // Продолжаем существующую передачу, если это переподключение того же клиента,
            // или перезапущенный клиент шлёт тот же файл, а прежнее соединение уже разорвано.
            bool resume = s->size == size && (reconnect || (!s->done && !s->peer));
            if (resume) {
                if (s->done) {
                    log_msg("сервер", "Повторный запрос завершённой передачи от %s — файл уже сохранён как %s", p.conn->peer().c_str(), s->final_name.c_str());
                    send_start_ack(p, id, s->size, ST_ALREADY_DONE, s->final_name);
                    return;
                }
                if (p.sess && p.sess != s) { p.closing = true; return; }
                if (s->peer && s->peer != &p) {              // старое соединение «зависло» — забираем сессию
                    Peer* old = s->peer;
                    old->sess = nullptr;
                    s->peer = nullptr;
                    old->closing = true;
                }
                s->peer = &p; p.sess = s;
                s->deadline = 0; s->nack_sent = false;
                s->peer_name = p.conn->peer();
                log_msg("сервер", "Возобновление приема от %s: %s, с позиции %llu из %llu байт", s->peer_name.c_str(), s->final_name.c_str(),
                        (unsigned long long)s->received, (unsigned long long)s->size);
                send_start_ack(p, id, s->received, ST_OK, s->final_name);
                return;
            }
            // Тот же файл отправляется ещё раз (повторно или параллельно) — это новая передача под новым id
            do { id = rng_(); } while (id == 0 || sessions_.count(id));
        }

        if (p.sess) { p.closing = true; return; }   // вторая новая передача по тому же соединению не поддерживается
        auto s = std::make_unique<Session>();
        s->id = id; s->size = size; s->orig_name = name; s->peer_name = p.conn->peer();
        s->t_start = now_ms();
        std::string err;
        if (!create_files(*s, err)) {
            log_msg("сервер", "Не удалось создать файл: %s", err.c_str());
            send_start_ack(p, id, 0, ST_IO_ERROR, "");
            p.closing = true;
            return;
        }
        s->peer = &p;
        p.sess = s.get();
        log_msg("сервер", "Начало приема от %s: «%s», %llu байт -> %s", s->peer_name.c_str(), name.c_str(),
                (unsigned long long)size, s->final_name.c_str());
        send_start_ack(p, id, 0, ST_OK, s->final_name);
        sessions_[id] = std::move(s);
    }

    void on_data(Peer& p, const Frame& f) {
        Session* s = p.sess;
        if (!s || s->done) return;
        Reader r(f.payload.data(), f.payload.size());
        uint64_t off = r.u64();
        if (!r.ok()) return;
        const uint8_t* d = r.rest();
        size_t len = r.left();

        if (off == s->received) {
            if (s->received + len > s->size) { abort_session(p, ST_BAD_SIZE, "получено больше заявленного размера"); return; }
            size_t done = 0;
            while (done < len) {
                ssize_t w = pwrite(s->file.get(), d + done, len - done, static_cast<off_t>(off + done));
                if (w < 0) {
                    if (errno == EINTR) continue;
                    abort_session(p, ST_IO_ERROR, strerror(errno));
                    return;
                }
                done += static_cast<size_t>(w);
            }
            s->received += len;
            s->crc = crc32(d, len, s->crc);
            s->nack_sent = false;
            s->nack_pending = false;
            p.ack_pending = true;
        } else if (off > s->received) {                  // пропуск: просим повторить, один раз на «дырку»
            nack_or_defer(p, s);
        }                                                // off < received — дубликат, игнорируем
    }

    void on_end(Peer& p, const Frame& f) {
        Session* s = p.sess;
        if (!s) { send_done(p, ST_PROTOCOL, ""); p.closing = true; return; }
        Reader r(f.payload.data(), f.payload.size());
        uint64_t total = r.u64();
        uint32_t crc = r.u32();
        if (!r.ok()) { p.closing = true; return; }
        if (s->done) { send_done(p, ST_OK, s->final_name); return; }   // повтор END
        if (s->received < s->size) { send_nack(p, s); return; }
        if (total != s->size) { abort_session(p, ST_BAD_SIZE, "размер не совпал"); return; }
        if (crc != s->crc) { abort_session(p, ST_BAD_CRC, "контрольная сумма файла не совпала"); return; }

        fdatasync(s->file.get());
        s->file.reset();
        if (rename(s->part_path.c_str(), s->final_path.c_str()) < 0) {
            std::string e = strerror(errno);
            abort_session(p, ST_IO_ERROR, e.c_str());
            return;
        }
        s->done = true;
        s->deadline = now_ms() + SERVER_DONE_TTL;
        double sec = static_cast<double>(std::max<int64_t>(now_ms() - s->t_start, 1)) / 1000.0;
        log_msg("сервер", "Прием завершен: %s (%llu байт, %.1f с, %.2f МБ/с)", s->final_name.c_str(),
                (unsigned long long)s->size, sec, static_cast<double>(s->size) / sec / 1048576.0);
        send_done(p, ST_OK, s->final_name);
    }

    void abort_session(Peer& p, uint8_t status, const char* why) {
        Session* s = p.sess;
        log_msg("сервер", "Прием %s от %s прерван: %s", s->final_name.c_str(), s->peer_name.c_str(), why);
        destroy_session(s, true);
        send_done(p, status, "");
        p.closing = true;
    }

    void destroy_session(Session* s, bool remove_part) {
        if (remove_part && !s->done) unlink(s->part_path.c_str());
        if (s->peer) s->peer->sess = nullptr;
        uint64_t id = s->id;   // ключ копируем: erase уничтожает *s (и закрывает файл)
        sessions_.erase(id);
    }

    // Остатки от аварийно завершённого сервера
    void cleanup_stale_parts() {
        struct DirCloser { void operator()(DIR* p) const { closedir(p); } };
        std::unique_ptr<DIR, DirCloser> d(opendir(dir_.c_str()));
        if (!d) return;
        int n = 0;
        while (dirent* e = readdir(d.get())) {
            std::string nm = e->d_name;
            if (nm.size() > 9 && nm.compare(nm.size() - 9, 9, ".hex.part") == 0 && unlink((dir_ + "/" + nm).c_str()) == 0) ++n;
        }
        if (n) log_msg("сервер", "Удалено незавершённых файлов от прошлого запуска: %d", n);
    }

    IAcceptor& acc_;
    const LinkParams lp_;
    std::string dir_;
    UniqueFd ep_, sigfd_;
    std::map<int, std::unique_ptr<Peer>> peers_;
    std::map<uint64_t, std::unique_ptr<Session>> sessions_;
    std::mt19937_64 rng_{std::random_device{}()};
};

}  // namespace

int run_server(IAcceptor& acceptor, const std::string& dir) {
    Server server(acceptor, dir);
    return server.run();
}

}  // namespace filexfer
