#!/usr/bin/env bash
# Интеграционные тесты filexfer (Linux).
# Запуск:  tests/run_tests.sh ./filexfer      или      make test
#
# Сервер сохраняет файлы рядом со своим исполняемым файлом, поэтому бинарник
# копируется во временный каталог tests/tmp/srv и все результаты оказываются там.

set -u

BIN=$(realpath "${1:-./filexfer}")
ROOT=$(cd "$(dirname "$0")" && pwd)
WORK="$ROOT/tmp"
SRV_DIR="$WORK/srv"
SRV="$SRV_DIR/filexfer"
PORT=$(( 20000 + RANDOM % 20000 ))
SERVER_PID=
PASS=0 FAIL=0 SKIP=0

rm -rf "$WORK"
mkdir -p "$SRV_DIR" "$WORK/in"
cp "$BIN" "$SRV"

pass() { echo "  OK    $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL  $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP  $1"; SKIP=$((SKIP + 1)); }

start_server() {
    "$SRV" -s "$PORT" > "$WORK/server.log" 2>&1 &
    SERVER_PID=$!
    for _ in $(seq 100); do
        grep -q "Ожидание" "$WORK/server.log" 2>/dev/null && return 0
        sleep 0.05
    done
    echo "Сервер не запустился:"; cat "$WORK/server.log"; exit 1
}

stop_server() {
    if [ -n "$SERVER_PID" ]; then
        kill "$SERVER_PID" 2>/dev/null
        wait "$SERVER_PID" 2>/dev/null
        SERVER_PID=
    fi
}
trap stop_server EXIT

client() { timeout 120 "$SRV" -c "$1" 127.0.0.1 "$PORT"; }

# Имя, под которым сервер сохранил файл (клиент пишет его в последней строке лога)
saved_name() { sed -n 's/.*сохранил файл как \(.*\)$/\1/p' "$1" | tail -1; }

# send_and_check <описание> <файл>
send_and_check() {
    local log="$WORK/client_$RANDOM$RANDOM.log" name
    if ! client "$2" > "$log" 2>&1; then
        fail "$1: клиент завершился с ошибкой"; sed 's/^/        /' "$log"; return 1
    fi
    name=$(saved_name "$log")
    if [ -n "$name" ] && cmp -s "$2" "$SRV_DIR/$name"; then
        pass "$1 -> $name"
    else
        fail "$1: файл на сервере не совпадает с исходным ('$name')"; return 1
    fi
    LAST_LOG=$log LAST_NAME=$name
}

cpu_ticks() { awk '{ print $14 + $15 }' "/proc/$SERVER_PID/stat"; }

start_server

echo "== Обязательные требования"

"$SRV" > /dev/null 2>&1
[ $? -eq 2 ] && pass "без аргументов — подсказка и код 2" || fail "без аргументов — ожидался код 2"

head -c 1M /dev/urandom > "$WORK/in/basic.bin"
if send_and_check "передача 1 МиБ" "$WORK/in/basic.bin"; then
    [[ $LAST_NAME =~ ^[0-9]{8}_[0-9]{6}(_[0-9]+)?\.hex$ ]] \
        && pass "имя файла в формате date_time.hex" || fail "имя файла '$LAST_NAME' не в формате date_time.hex"
    grep -q "Начало передачи" "$LAST_LOG" && grep -q "Передача завершена" "$LAST_LOG" \
        && pass "клиент сообщает о начале и завершении" || fail "клиент не сообщил о начале/завершении"
    grep -q "Начало приема" "$WORK/server.log" && grep -q "Прием завершен" "$WORK/server.log" \
        && pass "сервер сообщает о начале и завершении" || fail "сервер не сообщил о начале/завершении"
fi

: > "$WORK/in/empty.bin"
send_and_check "пустой файл" "$WORK/in/empty.bin"

head -c 100K /dev/urandom > "$WORK/in/имя с пробелами.bin"
send_and_check "имя файла с пробелами и кириллицей" "$WORK/in/имя с пробелами.bin"

# В тексте задания ключ клиента набран кириллической «с» — проверяем и его
log="$WORK/cyr.log"
if timeout 60 "$SRV" $'-\xd1\x81' "$WORK/in/basic.bin" 127.0.0.1 "$PORT" > "$log" 2>&1 \
    && cmp -s "$WORK/in/basic.bin" "$SRV_DIR/$(saved_name "$log")"; then
    pass "ключ клиента «-с» (кириллица)"
else
    fail "ключ клиента «-с» (кириллица)"
fi

client "$WORK/in/no_such_file" > /dev/null 2>&1
[ $? -eq 1 ] && pass "несуществующий файл — код 1" || fail "несуществующий файл — ожидался код 1"

client "$WORK/in" > /dev/null 2>&1
[ $? -eq 1 ] && pass "каталог вместо файла — код 1" || fail "каталог вместо файла — ожидался код 1"

head -c 64M /dev/urandom > "$WORK/in/big.bin"
send_and_check "передача 64 МиБ" "$WORK/in/big.bin"

echo "== Дополнительные требования"

# Несколько клиентов одновременно, разные файлы
pids=()
for i in $(seq 1 8); do
    head -c $((RANDOM * 64 + i)) /dev/urandom > "$WORK/in/par_$i.bin"
    client "$WORK/in/par_$i.bin" > "$WORK/par_$i.log" 2>&1 &
    pids+=($!)
done
ok=1
for i in $(seq 1 8); do
    wait "${pids[$((i - 1))]}" || ok=0
    cmp -s "$WORK/in/par_$i.bin" "$SRV_DIR/$(saved_name "$WORK/par_$i.log")" || ok=0
done
[ $ok -eq 1 ] && pass "8 клиентов одновременно" || fail "8 клиентов одновременно"

# Один и тот же файл двумя клиентами одновременно — две независимые копии
head -c 8M /dev/urandom > "$WORK/in/same.bin"
client "$WORK/in/same.bin" > "$WORK/same_1.log" 2>&1 & p1=$!
client "$WORK/in/same.bin" > "$WORK/same_2.log" 2>&1 & p2=$!
wait $p1; r1=$?; wait $p2; r2=$?
n1=$(saved_name "$WORK/same_1.log"); n2=$(saved_name "$WORK/same_2.log")
if [ $r1 -eq 0 ] && [ $r2 -eq 0 ] && [ -n "$n1" ] && [ "$n1" != "$n2" ] \
    && cmp -s "$WORK/in/same.bin" "$SRV_DIR/$n1" && cmp -s "$WORK/in/same.bin" "$SRV_DIR/$n2"; then
    pass "один файл двумя клиентами одновременно — две копии"
else
    fail "один файл двумя клиентами одновременно ($n1 / $n2)"
fi

# Повторная отправка того же файла после успешной — новый файл, а не «уже принят»
send_and_check "повторная отправка уже переданного файла" "$WORK/in/basic.bin"

# Канал с помехами в обе стороны (FILEXFER_FAULTS): байты теряются и искажаются и на отправке, и на приёме.
# Протокол должен обнаружить это по CRC, запросить повтор и доставить файл без искажений.
head -c 1M /dev/urandom > "$WORK/in/noisy.bin"
log="$WORK/noisy.log"
if FILEXFER_FAULTS="drop=0.00003,flip=0.00003,seed=42,dir=both" timeout 180 "$SRV" -c "$WORK/in/noisy.bin" 127.0.0.1 "$PORT" > "$log" 2>&1 \
    && cmp -s "$WORK/in/noisy.bin" "$SRV_DIR/$(saved_name "$log")"; then
    retx=$(sed -n 's/.*повторов: \([0-9]*\).*/\1/p' "$log" | tail -1)
    [ "${retx:-0}" -gt 0 ] && pass "канал с потерей и искажением байтов в обе стороны: файл цел, повторов: $retx" \
                           || fail "канал с помехами: повторов не было — помехи не сработали?"
else
    fail "канал с потерей и искажением байтов"; sed 's/^/        /' "$log" | tail -20
fi

# Клиент без сервера не должен долго ждать
SECONDS=0
timeout 30 "$SRV" -c "$WORK/in/basic.bin" 127.0.0.1 $((PORT + 1)) > "$WORK/noserver.log" 2>&1
rc=$?
if [ $rc -eq 1 ] && [ $SECONDS -le 5 ] && grep -q "недоступен" "$WORK/noserver.log"; then
    pass "сервер недоступен — клиент сразу сообщает об этом (код 1, $SECONDS с)"
else
    fail "сервер недоступен: код $rc за $SECONDS с"
fi

# interrupt_midway <сигнал> <файл> <лог> — запускает клиента и посылает ему сигнал, когда сервер
# уже принял часть файла. Код 0 — прервали посреди передачи, 1 — не успели. Код клиента — в INTERRUPT_RC.
# Клиент запускается напрямую, а не через функцию client: иначе $! был бы pid подоболочки.
interrupt_midway() {
    local sig=$1 file=$2 log=$3 pid part=
    "$SRV" -c "$file" 127.0.0.1 "$PORT" > "$log" 2>&1 &
    pid=$!
    for _ in $(seq 6000); do
        part=$(ls "$SRV_DIR"/*.hex.part 2>/dev/null | head -1)
        if [ -n "$part" ] && [ "$(stat -c %s "$part" 2>/dev/null || echo 0)" -gt 0 ]; then
            kill "-$sig" "$pid" 2>/dev/null
            break
        fi
        kill -0 "$pid" 2>/dev/null || break
        sleep 0.005
    done
    { wait "$pid"; } 2>/dev/null   # без сообщения bash «Killed»
    INTERRUPT_RC=$?
    [ -n "$part" ] && ! grep -q "Передача завершена" "$log"
}

# check_resume <описание> <файл> — повторный запуск должен продолжить передачу, а не начать заново
check_resume() {
    sleep 0.3
    if send_and_check "$1" "$2"; then
        grep -q "продолжаем с этого места" "$LAST_LOG" \
            && pass "$1: передача продолжена с места обрыва" \
            || fail "$1: передача началась заново вместо докачки"
    fi
}

head -c 512M /dev/urandom > "$WORK/in/resume.bin"

# Ctrl+C клиента: понятное сообщение, код 130, затем докачка
if interrupt_midway INT "$WORK/in/resume.bin" "$WORK/sigint.log"; then
    if [ $INTERRUPT_RC -eq 130 ] && grep -q "Передача прервана" "$WORK/sigint.log"; then
        pass "Ctrl+C клиента — сообщение о прерывании и код 130"
    else
        fail "Ctrl+C клиента: код $INTERRUPT_RC"; sed 's/^/        /' "$WORK/sigint.log" | tail -5
    fi
    check_resume "докачка после Ctrl+C" "$WORK/in/resume.bin"
else
    skip "Ctrl+C клиента: передача завершилась раньше, чем её удалось прервать"
fi

# kill -9 клиента: процесс убит без предупреждения, повторный запуск всё равно докачивает.
# touch меняет время изменения — для сервера это новый файл, а не повтор уже принятого.
touch "$WORK/in/resume.bin"
if interrupt_midway KILL "$WORK/in/resume.bin" "$WORK/sigkill.log"; then
    check_resume "докачка после kill -9" "$WORK/in/resume.bin"
else
    skip "kill -9 клиента: передача завершилась раньше, чем её удалось прервать"
fi
rm -f "$WORK/in/resume.bin" "$SRV_DIR"/*.hex.part

# Сервер в ожидании не должен нагружать процессор
sleep 0.5
t1=$(cpu_ticks); sleep 3; t2=$(cpu_ticks)
[ $((t2 - t1)) -le 1 ] && pass "простой сервера 3 с: $((t2 - t1)) тиков CPU" \
                       || fail "сервер тратит CPU в простое: $((t2 - t1)) тиков за 3 с"

echo "== Завершение"

kill -TERM "$SERVER_PID"
wait "$SERVER_PID"; rc=$?
SERVER_PID=
[ $rc -eq 0 ] && grep -q "Остановка" "$WORK/server.log" \
    && pass "сервер корректно завершается по SIGTERM" || fail "сервер по SIGTERM: код $rc"

ls "$SRV_DIR"/*.hex.part > /dev/null 2>&1 && fail "остались временные .part файлы" || pass "временных .part файлов не осталось"

if grep -lE "AddressSanitizer|LeakSanitizer|runtime error:" "$WORK"/*.log > /dev/null 2>&1; then
    fail "санитайзеры нашли ошибки:"; grep -hE -A20 "AddressSanitizer|LeakSanitizer|runtime error:" "$WORK"/*.log | head -60
fi

echo
echo "Итого: пройдено $PASS, ошибок $FAIL, пропущено $SKIP"
[ $FAIL -eq 0 ] && rm -rf "$WORK"
[ $FAIL -eq 0 ]
