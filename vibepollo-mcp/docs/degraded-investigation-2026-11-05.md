# Vibepollo: методика расследования `degraded`-сессий

Дата фиксации: **2026-11-05**  
Последняя проверка: **2026-11-05, Asia/Tbilisi**  
Режим: read-only, конфигурация и код не изменялись.

Этот документ сохраняет не только вывод, но и порядок измерений, чтобы следующую
проверку можно было повторить без восстановления контекста.

## 1. Топология и исходные условия

```text
client:  192.168.1.153, wlp114s0, Wi-Fi, ath11k_pci
router:  192.168.1.1, OpenWrt 24.10.5
host:    192.168.1.158, Vibepollo/Windows
SSID:    kolenka
Wi-Fi:   5 GHz, channel 36, 80 MHz
client:  /home/nsmirnov/gh/thirdparty/moonlight-qt/app/moonlight
```

Использованный контрольный запуск:

```bash
/home/nsmirnov/gh/thirdparty/moonlight-qt/app/moonlight \
  stream nspc Desktop --1080 --bitrate 100000
```

`100000` — запрошенный битрейт в kbps, а не доказательство фактической
скорости передачи каждого отдельного интервала.

## 2. Что именно означает `degraded`

Установленный Vibepollo:

```text
commit: 04a354c115dff847becaf71fddf1034cd8efd19e
version: 1.18.3-stable.2
source checkout used for audit: /tmp/vibepollo-src.NILJ2W
```

В `src/session_history_storage.cpp`, строки 806–831, verdict вычисляется так:

```cpp
if (loss_ratio > 0.05) {
  verdict = "failed";
} else if (high_latency_count > 0 || max_reported_losses > 0) {
  verdict = "degraded";
} else {
  verdict = "healthy";
}
```

При этом:

```sql
high_latency_count =
  SUM(CASE WHEN encode_latency_ms > 16 THEN 1 ELSE 0 END)
```

Следовательно, `degraded` означает наличие хотя бы одного из условий:

1. `encode_latency_ms > 16 ms`;
2. клиент сообщил хотя бы одну потерю.

Это **не равнозначно** «плохой Wi-Fi». Verdict не доказывает конкретный
нижний уровень причины.

## 3. Как трактовать `encode_latency_ms`

В `src/stream.cpp`, примерно строки 2021–2031:

```cpp
now - packet->host_processing_timestamp
```

Результат записывается в заголовок видеокадра и сохраняется как
`encode_latency_ms`.

Это показатель задержки host-side обработки/очереди, а не обязательно чистое
время вызова NVENC. В него потенциально могут попасть:

```text
Windows capture/WGC
  -> frame queue
  -> encoder
  -> packetization
  -> задержка stream send-thread
```

## 4. Команды и порядок измерений

### 4.1. Снять состояние Vibepollo

Через MCP сначала выполнить безопасные read-only вызовы:

```text
vibepollo_get_metadata
vibepollo_get_config
vibepollo_get_sessions
vibepollo_get_logs
vibepollo_webrtc_capabilities
```

Для ручного API-запроса использовать только `/api/...` и не помещать токены в
команду или документ:

```bash
export VIBEPOLLO_URL='https://192.168.1.158:47990'
export VIBEPOLLO_API_TOKEN='[REDACTED]'
export VIBEPOLLO_VERIFY_TLS=false

curl --fail --silent --show-error \
  -H "Authorization: Bearer ${VIBEPOLLO_API_TOKEN}" \
  "${VIBEPOLLO_URL}/api/history/sessions"
```

Если endpoint истории отличается в текущей версии, сначала получить live
каталог маршрутов через `vibepollo_get_route_catalog`, затем использовать
фактический read-only history endpoint. Не считать README или старую схему
доказательством наличия маршрута.

Нужные поля истории:

```text
session UUID
start/end
codec, resolution, target FPS, requested bitrate
verdict
encode_latency_ms samples/statistics
client_reported_losses / max_reported_losses
video_dropped, audio_dropped, ref_invalidations
IDR requests
```

### 4.2. Проверить клиентский Wi-Fi до теста

```bash
iw dev wlp114s0 link
iw dev wlp114s0 get power_save
ethtool -i wlp114s0
ip -s link show dev wlp114s0
```

Сохранить отдельно:

```text
SSID/BSSID, frequency/channel, width
signal
tx bitrate
driver/firmware
RX/TX packets, errors, dropped
power-save state
```

### 4.3. Проверить AP и радио на OpenWrt

```bash
ssh root@192.168.1.1

iw dev phy1-ap0 station dump
ubus call network.wireless status
```

Из `station dump` сохранить до и после теста:

```text
signal / signal-avg
tx bitrate
rx/tx packets
tx retries
tx failed
beacon loss
inactive time
```

Считаются дельты, а не только абсолютные значения:

```text
delta_tx_retries = after.tx_retries - before.tx_retries
delta_tx_failed  = after.tx_failed  - before.tx_failed
delta_tx_packets = after.tx_packets  - before.tx_packets
```

### 4.4. Измерить задержку до хоста параллельно со стримом

На клиенте во время всей сессии:

```bash
ping -D -i 0.2 -c 500 192.168.1.158 | tee /tmp/vibepollo-ping.txt
```

Зафиксировать:

```text
packet loss
min/avg/max RTT
временные пики RTT
```

`ping` показывает сетевой jitter/потери до хоста, но не заменяет историю
Vibepollo и не измеряет encode/queue latency напрямую.

### 4.5. Провести одинаковые контрольные сессии

Минимум:

```text
H.264, 1920x1080, 60 FPS, bitrate 100000 kbps
HEVC, 1920x1080, 60 FPS, bitrate 100000 kbps
```

Для каждого запуска сохранить UUID, время, codec, verdict, loss и latency.
Условия должны быть одинаковыми: тот же клиент, AP, SSID, канал, host,
разрешение, FPS и bitrate.

### 4.6. При необходимости проверить host-side ресурсы

Снимать в те же временные интервалы, что и latency spikes:

```text
CPU total/process
GPU utilization
GPU encoder utilization
RAM/VRAM
GPU temperature
host TX bitrate
```

Важно не делать вывод «ресурсы не причина» только по низкой средней загрузке:
короткий queue/scheduler stall может не отразиться в среднем проценте.

## 5. Результаты последнего расследования

### 5.1. Исходная `degraded`-сессия

```text
UUID: 66DF6956-5774-B721-2800-3A813BEC2795
Время: 2026-11-04 23:46:01.995–23:48:05.312 Asia/Tbilisi
transport: RTSP
codec: H.264
resolution: 1920x1080
target FPS: 60
requested bitrate: 100000 kbps
duration: 123.3 s
verdict: degraded
```

Ключевые наблюдения:

```text
encode_latency_ms spikes: 104.6 ms, 212.0 ms
client_reported_losses: 0
video_dropped: 0
audio_dropped: 0
ref_invalidations: 0
IDR requests: 2 -> 12 -> 20 -> 25
host CPU: approximately 4–10%
GPU: approximately 3–7%
GPU encoder: approximately 13–18%
RAM: approximately 29.6%

### 5.2. Повторный тест через Ethernet, IP `192.168.1.209`

Дата: **2026-11-05, Asia/Tbilisi**  
Интерфейс: `enp0s13f0u3`  
Драйвер: `r8152`, USB `usb-0000:00:0d.0-3`  
Link: **100 Mb/s, Full Duplex**  
Маршрут перед запуском:

```text
192.168.1.158 dev enp0s13f0u3 src 192.168.1.209
```

Маршрут был установлен явно:

```bash
ip route replace 192.168.1.158/32 dev enp0s13f0u3 src 192.168.1.209
```

Проверка канала: `ping -I enp0s13f0u3 -c 10 -W 1 192.168.1.158` дал
`0% packet loss`, средний RTT `1.222 ms`, максимум `2.130 ms`.

Запуск:

```bash
/home/nsmirnov/gh/thirdparty/moonlight-qt/app/moonlight \
  --video-codec HEVC --1080 --fps 60 --bitrate 100000 \
  stream nspc Desktop
```

Результат Vibepollo:

```text
UUID: DC0853F7-74E2-8000-2399-1DD7F467BB42
duration: 152.6 s
codec: HEVC
resolution/FPS: 1920x1080 @ 60
requested bitrate: 100000 kbps
encoder bitrate: 78988 kbps
verdict: healthy
total samples: 76
max observed encode_latency_ms: 6.6 ms
client_reported_losses: 0
video_dropped/audio_dropped: 0/0
ref_invalidations: 6
IDR requests: 2
history_status: available=true, dropped_samples=0, failed_writes=0
```

Оконечный клиентский лог при остановке по `timeout 150s` зафиксировал
сетевые потери последних кадров и RFI, но это не изменило сохранённый verdict:
Vibepollo не получил `client_reported_losses`, а `encode_latency_ms` не превысил
порог `16 ms`. `iftop` в текущем запуске не смог открыть pcap без
`CAP_NET_RAW`; путь подтверждён маршрутом, IP-интерфейсом и счётчиками
`enp0s13f0u3` (дельта TX `655573` байт, RX `947867485` байт).

### 5.3. Вывод по причине `degraded`

Подтверждено:

1. Предыдущая проблемная сессия выполнялась не только на Wi-Fi: аналогичный
   `degraded` уже наблюдался на Ethernet.
2. `degraded` выставляется не за сам факт Wi-Fi или Ethernet, а за
   `encode_latency_ms > 16 ms` либо ненулевые потери, зарегистрированные
   Vibepollo.
3. Новый Ethernet-тест с теми же 1080p60/HEVC/100000 kbps при явном маршруте
   через `enp0s13f0u3` завершился `healthy`; типичный latency был около
   `2.4–4.4 ms`, максимум в сохранённых samples `6.6 ms`.
4. Интерфейс физически поднят только на 100 Mb/s. При фактическом TX около
   `90–98 Mb/s` запас практически отсутствует.

Наиболее вероятный механизм старого `degraded`: пиковый bitrate/очередь
отправки упираются в 100-Mb/s link, а в конфигурации
`pacing_max_bitrate_kbps=0` pacing не ограничивает поток под этот канал.
Возникает sender backpressure, растёт host-side queue latency, и один или
несколько samples пересекают порог `16 ms`. Это согласуется с историческими
spikes до `110.1 ms`, но не доказывает, что каждый spike вызван только
сетевой очередью.

Не доказано этим тестом:

```text
- почему именно произошли 6 ref_invalidations в конце;
- что физический адаптер способен работать на 1 Gb/s;
- что iftop-показания были бы эквивалентны без CAP_NET_RAW.
```

## 6. Как локализовать проблему

Цель следующей серии — не просто получить ещё один `healthy`, а найти
условие, после которого появляется первый `encode_latency_ms > 16 ms` или
первая зарегистрированная потеря. За один запуск менять только один фактор.

### Шаг 1. Сначала добиться воспроизводимого `degraded`

Нужен динамичный сценарий на рабочем столе, иначе фактический bitrate может
остаться ниже предела канала и проблема не проявится. Для каждого запуска
сохранять:

```text
UUID, transport, interface, route, codec, resolution, FPS
requested/actual bitrate, verdict
max/p95 encode_latency_ms
host_net_tx_bps
client losses, ref_invalidations, video/audio drops
```

Тест считать воспроизведённым только при подтверждении истории Vibepollo, а не
по картинке или exit code клиента.

### Шаг 2. Разделить transport bottleneck и host-side bottleneck

Провести одну и ту же динамичную сцену в четырёх вариантах:

| Вариант | Transport | Pacing | Что проверяет |
|---|---|---:|---|
| A | текущий 100-Mb/s Ethernet | `0` | baseline |
| B | текущий 100-Mb/s Ethernet | `85000` | исчезает ли queue backpressure |
| C | подтверждённый Gigabit Ethernet | `0` | исчезает ли ограничение 100 Mb/s |
| D | Gigabit Ethernet | `85000` | контроль влияния pacing |

Интерпретация:

```text
A degraded, B healthy, C healthy:
  сильное подтверждение saturation/backpressure на 100-Mb/s link.

A degraded, B degraded, C degraded:
  искать capture/NVENC/scheduler/send queue на host; сеть не доказана как
  единственная причина.

A/B degraded, C healthy:
  основная причина — пропускная способность или burstiness текущего адаптера.

Все варианты degraded:
  сравнить host CPU/GPU encoder, capture latency, driver и содержимое сцены.
```

### Шаг 3. Проверить, где именно появляется задержка

При каждом запуске одновременно собирать:

```bash
watch -n 1 'ip -s link show dev enp0s13f0u3'
```

и Vibepollo samples через:

```text
GET /api/history/sessions/active
GET /api/history/sessions/<UUID>
```

Если доступен root на клиенте, вместо `iftop` использовать:

```bash
sudo tcpdump -ni enp0s13f0u3 host 192.168.1.158
```

`iftop`/tcpdump должны подтверждать поток именно через `enp0s13f0u3`; одного
наличия IP на интерфейсе недостаточно. Для численной сверки использовать
дельту `tx_bytes` интерфейса и `host_net_tx_bps` из истории Vibepollo.

### Шаг 4. Отделить потери сети от host queue latency

Сопоставлять временные ряды по timestamp:

```text
encode_latency_ms > 16 ms
host_net_tx_bps близок к 100 Mb/s
client losses/ref_invalidations
interface errors/dropped/overruns
```

Признаки:

```text
latency spike без client loss, при TX около лимита:
  sender queue/backpressure.

client loss + ref_invalidations + interface drops/errors:
  сетевой путь или адаптер.

latency spike при TX далеко ниже лимита и без interface errors:
  capture/NVENC/scheduler/приложение на host.
```

### Шаг 5. Сравнить кодеки без изменения transport

На одном и том же Ethernet-пути повторить:

```text
H.264 1920x1080@60, 100000 kbps
HEVC  1920x1080@60, 100000 kbps
```

Если проблема появляется только у одного кодека при одинаковом фактическом
TX, проверить NVENC preset, driver и encoder queue. Если оба кодека ломаются
только около лимита интерфейса, приоритет остаётся за transport/pacing.

### Практический следующий тест

Сейчас самый дешёвый информативный A/B:

1. Оставить маршрут через `enp0s13f0u3`.
2. Создать динамичную сцену, чтобы фактический TX держался около
   `90–98 Mb/s`.
3. Запустить 120–150 секунд с `pacing_max_bitrate_kbps=0`.
4. Повторить с `pacing_max_bitrate_kbps=85000`.
5. В обоих случаях проверить историю, а затем вернуть значение `0`.

Этот тест локализует именно влияние pacing на 100-Mb/s канале. Но он не
заменяет тест на Gigabit: без него нельзя окончательно отделить ограничение
адаптера от других sender-side причин.

### 6.1. Выполненный A/B без Gigabit-адаптера

Оба запуска выполнены через один и тот же маршрут:

```text
192.168.1.158 dev enp0s13f0u3 src 192.168.1.209
```

Параметры обоих запусков:

```text
HEVC, 1920x1080@60, requested bitrate 100000 kbps
duration: 147.1 s
```

#### A: `pacing_max_bitrate_kbps=0`

```text
UUID: 18AA8CD8-EC13-0E0C-5A9C-0418632A4A2B
verdict: healthy
max encode_latency_ms: 5.6 ms
client_reported_losses: 0
video/audio drops: 0/0
ref_invalidations: 6
history samples: 73, dropped samples: 0
```

Клиентский итог: host processing latency `2.3/10.2/3.1 ms`
(min/max/average), network drops `0.12%`.

#### B: `pacing_max_bitrate_kbps=85000`

```text
UUID: 745E01DA-AE3B-A000-4282-9E0C5DAA54ED
verdict: degraded
max encode_latency_ms: 384.2 ms
client_reported_losses: 0
video/audio drops: 0/0
ref_invalidations: 6
history samples: 73, dropped samples: 0
```

Зафиксированы latency spikes:

```text
58.2, 59.7, 138.2, 222.1, 384.2, 152.5,
89.1, 33.6, 232.2, 117.1, 91.2, 73.1,
27.3 ms
```

Клиентский итог: host processing latency `2.3/549.8/36.0 ms`
(min/max/average), network drops `0.79%`.

После A/B значение было возвращено и проверено:

```json
{"pacing_max_bitrate_kbps": 0}
```

#### Интерпретация A/B

Этот результат **не подтверждает**, что значение `85000` лечит проблему.
Наоборот, в конкретной динамичной сцене оно стало хуже: `85000` дало
`degraded`, а `0` — `healthy`. Однако это не означает, что pacing является
первопричиной: pacing меняет форму burst/очереди, а не только средний bitrate.

Сейчас подтверждено более узко:

```text
degraded появляется при sender/host-side latency spikes;
потери клиента не обязательны;
динамичная сцена действительно способна воспроизвести spikes;
на 100-Mb/s канале результат зависит от конкретного burst-профиля сцены.
```

Следующий полезный тест без нового оборудования — повторить тот же A/B ещё
раз с тем же динамичным сценарием и сохранить полные samples обоих запусков.
Если `85000` снова ухудшает результат, его следует считать неподходящим
workaround для этой конфигурации; затем проверить промежуточные значения
например `60000` и `70000`, меняя только pacing.

## 7. Десятиминутное сравнение Wi-Fi и Ethernet

Дата: **2026-11-05, Asia/Tbilisi**  
Общие параметры обоих запусков:

```text
HEVC, 1920x1080@60, requested bitrate 100000 kbps
duration: около 607 s
pacing_max_bitrate_kbps: 0
```

### 7.1. Wi-Fi

Перед запуском маршрут был подтверждён:

```text
192.168.1.158 dev wlp114s0 src 192.168.1.153
```

```text
UUID: BF549DDE-CE1A-F229-4A95-2A9DF76503C7
duration: 607.2 s
verdict: healthy
history samples: 303
client_reported_losses: 0
video/audio drops: 0/0
max encode_latency_ms: 4.8 ms
ref_invalidations: 133
IDR requests: 2
```

Итог Moonlight:

```text
host processing latency: 2.3/11.8/3.0 ms
frames dropped by network connection: 1.33%
frames dropped by network jitter: 0.00%
average network latency: 9 ms
variance: 4 ms
```

### 7.2. Ethernet

Перед запуском маршрут был переключён и подтверждён:

```text
192.168.1.158 dev enp0s13f0u3 src 192.168.1.209
```

```text
UUID: A3757359-2ABD-5003-639C-D62193B556B0
duration: 607.3 s
verdict: healthy
history samples: 303
client_reported_losses: 0
video/audio drops: 0/0
max encode_latency_ms: 4.1 ms
ref_invalidations: 3
IDR requests: 2
```

Итог Moonlight:

```text
host processing latency: 2.3/16.5/3.2 ms
frames dropped by network connection: 0.01%
frames dropped by network jitter: 0.00%
average network latency: 2 ms
variance: 4 ms
```

### 7.3. Сравнение

```text
                         Wi-Fi              Ethernet
route                    wlp114s0           enp0s13f0u3
source IP                192.168.1.153      192.168.1.209
duration                  607.2 s            607.3 s
Vibepollo verdict         healthy            healthy
max encode latency        4.8 ms             4.1 ms
client losses             0                  0
Moonlight network drops   1.33%              0.01%
avg network latency       9 ms               2 ms
ref invalidations         133                3
```

Оба десятиминутных теста завершились `healthy`. Ethernet заметно лучше по
сетевым потерям, задержке и числу ref invalidations, но именно критерий
Vibepollo `degraded` в этих двух запусках не сработал. Поэтому вывод:

```text
Wi-Fi ухудшает качество transport.
Однако сам факт degraded не объясняется обычной потерей Wi-Fi-пакетов:
его trigger — редкие host-side encode/queue spikes, которые в этой серии
не появились.
```
VRAM: approximately 7%
GPU temperature: approximately 69 C
host TX: approximately 93–98 Mbps
```

### 5.4. Контрольные здоровые сессии на том же Wi-Fi

```text
H.264:
  F6CFB605-9415-0E0E-D794-5CC2F7EBB4E4
  duration 147.2 s, healthy, losses 0, latency approximately 3–4 ms

H.264:
  DF6F2092-D38F-A524-4D8E-D10F0A7FF6DD
  duration 62.1 s, healthy

HEVC:
  BF242E1C-35D3-E20D-7590-EB31550D83A9
  duration 147.1 s, healthy, losses 0, drops 0, IDR requests 2
```

### 5.5. Наблюдения по Wi-Fi

```text
client signal: approximately -54…-60 dBm
AP-observed signal: approximately -65…-69 dBm
5 GHz, channel 36, 80 MHz
```

В одном high-bitrate H.264 тесте за время сессии:

```text
tx packets: +approximately 306,504
tx retries/failed: +approximately 48,929
```

Параллельный ping до host:

```text
500 packets
0% loss
min RTT: 1.485 ms
avg RTT: 7.942 ms
max RTT: 51.972 ms
```

## 6. Границы вывода

### Confirmed

- `degraded` в проверенной версии выставляется при `encode_latency_ms > 16`
  или клиентских потерях.
- В исходной сессии потери были нулевыми.
- В исходной сессии были пики host-processing latency 104.6 и 212.0 ms.
- Поэтому непосредственная причина verdict — превышение порога
  `encode_latency_ms`.
- Средняя загрузка CPU/GPU/encoder не была высокой.
- Та же Wi-Fi-среда успешно провела несколько здоровых 100 Mbps сессий,
  включая H.264 и HEVC.

### Inferred

- Наиболее точное описание события: кратковременный host-side
  processing/queue stall.
- Wi-Fi retransmit/jitter могли быть сопутствующим фактором, но имеющихся
  данных недостаточно, чтобы назвать их первичной причиной.

### Unknown

Не доказано, где именно возникли пики 104/212 ms:

```text
NVENC scheduling
WGC/display capture
frame queue
packetization/send queue
взаимодействие с Wi-Fi jitter
```

## 7. Короткий чек-лист следующего расследования

- [ ] Сохранить UUID, точное время и все параметры сессии.
- [ ] Получить verdict и samples из live history API.
- [ ] Проверить исходный код/commit, если версия Vibepollo изменилась.
- [ ] Выписать `encode_latency_ms > 16`, losses, drops, IDR.
- [ ] До/после снять `iw dev ... link`, `ip -s link`, AP `station dump`.
- [ ] Параллельно записать ping до `192.168.1.158`.
- [ ] Снять host CPU/GPU/encoder/RAM/VRAM/temperature/TX.
- [ ] Повторить минимум один идентичный H.264 control.
- [ ] Если проблема повторяется, провести wired A/B.
- [ ] Не называть Wi-Fi причиной без корреляции latency spike с радио/RTT
      и без сравнения wired control.

## 8. Выполненный проводной A/B (2026-11-05)

Проводной интерфейс клиента:

```text
interface: enp0s20f0u4u5c2
IP: 192.168.1.129/24
route to host: 192.168.1.158 dev enp0s20f0u4u5c2 src 192.168.1.129
link: 100 Mb/s, link detected: yes
```

Это важное ограничение: поток с фактической скоростью `~93–98 Mb/s` почти
полностью заполняет 100-Mb/s Ethernet. В первом запуске по Ethernet это
сопровождалось host `TX` около `98 Mb/s`, поэтому сам проводной A/B не был
одновременно A/B «без насыщения канала».

### 8.1. HEVC, 100 Mb/s Ethernet, requested 100000 kbps

```text
UUID: 6C0270E0-14A4-3A2D-1086-D527A55706E8
duration: 137.2 s
codec: HEVC
resolution/FPS: 1920x1080 / 60
requested bitrate: 100000 kbps
verdict: degraded
client_reported_losses: 0
```

Пики и сопутствующие признаки:

```text
encode_latency_ms: 23.8, 34.8, 33.0, 38.3,
                   85.3, 36.1, 42.1, 47.2,
                   92.0, 110.1 ms
ref_invalidations: 0 -> 7
idr_requests: 2
video/audio drops: 0/0
host TX: около 98 Mb/s на большей части сессии
host CPU/GPU/encoder: без насыщения
```

Это доказывает, что `degraded` воспроизводится и по Ethernet. Но при этом
путь был близок к пределу 100 Mb/s, поэтому возможна очередь отправки или
буферблоат. `ref_invalidations` — это сообщения клиента
`IDX_INVALIDATE_REF_FRAMES`, а не подтверждение потерь IP-пакетов.

### 8.2. H.264, те же параметры и Ethernet

```text
UUID: 3B0A7AF8-589E-0222-748F-D735BC5F64A9
duration: 147.1 s
codec: H.264
resolution/FPS: 1920x1080 / 60
requested bitrate: 100000 kbps
verdict: healthy
client_reported_losses: 0
video/audio drops: 0/0
```

Во время сессии `encode_latency_ms` оставался примерно `0–4.2 ms`, без
пороговых выбросов. Это ослабляет гипотезу «один только 100-Mb/s линк всегда
вызывает degraded», но не исключает взаимодействие насыщения с HEVC/профилем
кадров.

### 8.3. HEVC, тот же Ethernet, requested 50000 kbps

```text
UUID: FDBF3BD2-D05E-BD23-D100-DAE93BCD0120
duration: 127.1 s
codec: HEVC
resolution/FPS: 1920x1080 / 60
requested bitrate: 50000 kbps
encoder bitrate: 38988 kbps
verdict: healthy
```

В активной части теста:

```text
actual bitrate: около 45.7 Mb/s
encode latency: 3.1 ms
client losses: 0
ref invalidations: 0
IDR requests: 2
```

Это сильное evidence в пользу того, что комбинация **HEVC + высокая фактическая
скорость около предела 100-Mb/s Ethernet** является триггером или усилителем
проблемы. Это ещё не доказывает, что задержка возникает именно в сетевом
send queue: latency считается на host-side пути, а `encode_latency_ms` не
разделяет capture, encoder и send-thread.

### 8.4. Повторный HEVC на 100000 kbps

```text
UUID: 8CC16F5B-9D27-6320-B41C-E79E3C77A174
duration: 147.2 s
codec: HEVC
requested bitrate: 100000 kbps
verdict: healthy
```

Этот запуск был почти статичным: на наблюдаемом отрезке фактический поток был
около `7 Mb/s`, поэтому он не является полноценным повтором насыщенного
100-Mb/s сценария. Он показывает, что сам выбор HEVC недостаточен для
воспроизведения проблемы; важна нагрузка/содержимое или другой transient
фактор.

## 9. Обновлённый вывод после Ethernet

### Подтверждено

- Wi-Fi не является необходимым условием `degraded`: verdict повторился по
  Ethernet с IP `192.168.1.129`.
- Проводной H.264 на `100000 kbps` был healthy при тех же 1080p60.
- HEVC на фактических `~45.7 Mb/s` был healthy.
- Насыщенный HEVC-сценарий около `90–98 Mb/s` получил `degraded` с latency
  spikes и `ref_invalidations`.
- Потери, drops и ошибки Ethernet не являются обязательным условием verdict.

### Наиболее вероятная рабочая гипотеза

Проблема находится в сочетании:

```text
HEVC / характер кадров
  + фактический поток близко к пределу 100-Mb/s USB-Ethernet
  + временная задержка в host processing / packet send path
```

### Пока не доказано

- Возникает ли задержка в Windows capture/WGC, NVENC или очереди отправки.
- Является ли `ref_invalidations=7` причиной latency spike или реакцией клиента
  на уже возникший сбой.
- Исчезнет ли проблема на настоящем Gigabit Ethernet, где не будет
  100-Mb/s bottleneck.

### 9.1. Проверка host-логов

Для насыщенного HEVC-сеанса `6C0270E0-14A4-3A2D-1086-D527A55706E8` проверен
`GET /api/logs?tail=1000`. В интервале сессии нет сообщения о падении NVENC,
WGC, capture или display helper. Есть обычные строки создания
`hevc_nvenc`, подключения клиента и завершения сессии.

Повторяющееся:

```text
Warning: WSASendMsg() failed: 10022
```

появляется в начале и других сессий, включая healthy H.264/HEVC, поэтому
сейчас это не считается причиной именно `degraded`.

Ошибки:

```text
Couldn't receive data from udp socket:
No connection could be made because the target machine actively refused it
```

появляются при остановке теста через `timeout` и сопровождают
`CLIENT DISCONNECTED`; это артефакт принудительного завершения контрольного
запуска, а не доказательство причины latency spike.

## 9.2. Что доказал аудит send path в исходниках

В `src/stream.cpp` видеопоток отправляется отдельным
`stream::videoBroadcast` thread:

```text
video encoder -> video_packets queue -> videoBroadcastThread
                         packet pop -> frame header latency -> FEC
                         -> paced send_batch()/send()
```

`encode_latency_ms` записывается в frame header в `videoBroadcastThread`
сразу после `packet.pop()`. До этого packet получает
`packet_enqueue_timestamp` в `encode_nvenc()`. Поэтому задержка между
постановкой encoded frame в очередь и фактической обработкой sender thread
попадает в `encode_latency_ms`.

Критическая найденная деталь:

```cpp
if (pacing_max_bitrate_kbps > 0) {
  pacing_bps = pacing_max_bitrate_kbps * 1000;
} else {
  pacing_bps = 80% of 1 Gbps;
}
```

В `src/config.cpp` default:

```cpp
0,  // pacing_max_bitrate_kbps (0 = legacy 1 Gbps Ethernet assumption)
```

То есть текущий default-pacer рассчитан на 1-Gb/s Ethernet. При фактическом
100-Mb/s USB-Ethernet он не ограничивает поток под реальный link rate:
sender формирует burst, затем `send_batch()`/`send()` может блокироваться на
сетевом пути. Пока `videoBroadcastThread` занят отправкой, новые encoded
frames накапливаются в `video_packets`; после освобождения thread их
`encode_latency_ms` уже может быть десятки/сотни миллисекунд.

Это связывает наблюдаемые факты в одну причинную цепочку:

```text
100-Mb/s link saturation
  -> sender backpressure / queue delay
  -> packet pop delay
  -> encode_latency_ms > 16 ms
  -> verdict = degraded
```

Это уже сильнее, чем простая корреляция с битрейтом: механизм прямо виден в
исходном коде и объясняет, почему verdict срабатывает по host-side latency,
даже если `client_reported_losses == 0`.

## 9.3. Контролируемая проверка pacing

Для runtime-проверки 2026-11-05 временно применялось:

```json
{"pacing_max_bitrate_kbps": 85000}
```

API подтвердил:

```text
appliedNow: true
deferred: false
restartRequired: false
```

После этого был выполнен HEVC-тест по Ethernet:

```text
UUID: 21966B8D-0F24-5523-9887-14AD58EB8484
codec: HEVC
1920x1080@60
requested bitrate: 100000 kbps
duration: 147.2 s
verdict: healthy
client losses: 0
ref_invalidations: 0
video/audio drops: 0/0
encode_latency_ms: 0–3.0 ms
```

После теста настройка была возвращена:

```json
{"pacing_max_bitrate_kbps": 0}
```

с подтверждением `appliedNow: true`.

Ограничение этого runtime-теста: рабочий стол в этот запуск оказался
малодинамичным, фактическая скорость была в основном значительно ниже
`90–98 Mb/s`. Поэтому он подтверждает отсутствие побочных эффектов от
временного pacing, но не является полным повтором насыщенного HEVC-сценария.
Для окончательной runtime-проверки нужен динамичный экран/высокий фактический
битрейт при тех же двух настройках: `0` против `85000`.

## 9.4. Почему текущий Ethernet не может стать Gigabit

Проверка адаптера показала:

```text
driver: cdc_ether
USB model: USB_10_100_LAN
USB vendor/model: 1a86:5394
USB bus speed: 480M
Ethernet link: 100 Mb/s
```

Это не временный результат autonegotiation: устройство само является
USB 10/100 LAN адаптером и не сообщает Gigabit link modes. Текущий адаптер
нельзя использовать для чистого Gigabit A/B; для него нужен другой USB
Gigabit-адаптер и желательно подключение к порту/кабелю с подтверждённым
1000BASE-T.

## 10. Следующий наиболее полезный A/B

Для отделения Wi-Fi от host-side причины:

1. Оставить тот же host, client, codec, resolution, FPS и bitrate.
2. Провести один запуск по Ethernet.
3. Повторить один запуск по исходному Wi-Fi.
4. Сопоставить временные интервалы `encode_latency_ms`, ping и host metrics.

Интерпретация:

```text
spikes только по Wi-Fi:
  Wi-Fi/network path likely contributes; inspect queue/retransmit correlation.

spikes и по Ethernet:
  искать capture/encoder/host scheduling/send queue.

spikes исчезают без изменения transport:
  вероятен transient host/application event; повторить серию тестов.
```

Следующий чистый тест — **Gigabit Ethernet** или временное ограничение HEVC ниже
примерно `50–70 Mb/s` при динамичном содержимом. Если на Gigabit при высокой
фактической скорости снова появятся те же latency spikes, сетевое насыщение
100-Mb/s USB-адаптера будет исключено, и фокус надо переносить на
WGC/NVENC/очередь кадров. Если spikes исчезнут, bottleneck Ethernet станет
основной подтверждённой причиной.

## 11. Исследование `Frames dropped by your network connection` на Wi-Fi

### 11.1. Что именно считает Moonlight

В локальном исходнике Moonlight:

```text
app/streaming/video/ffmpeg.cpp:2129–2131
```

`networkDroppedFrames` увеличивается, когда очередной `du->frameNumber`
больше предыдущего номера на один или более. То есть показатель означает
**пропуск видеокадров по последовательности, замеченный декодером**, а не
прямой счётчик Wi-Fi ошибок и не обязательно число окончательно потерянных
802.11-пакетов.

Кадр может быть пропущен из-за UDP-потери, слишком позднего прихода после
retransmit/FEC, переполнения очереди клиента или восстановления после
пропущенного диапазона кадров.

### 11.2. Состояние Wi-Fi и роутера

Клиент:

```text
interface: wlp114s0
IP: 192.168.1.153
driver: ath11k_pci
power save: off
SSID: kolenka
frequency: 5180 MHz, 80 MHz
client signal: approximately -55 dBm
client TX/RX bitrate: approximately 1080/1200 Mbit/s
client interface errors/dropped: 0/0
```

Роутер: OpenWrt `24.10.5`, Xiaomi Mi Router AX3000T. Для станции
`2a:e9:f2:9e:b3:6d`:

```text
signal: approximately -62…-63 dBm
last/average ACK signal: approximately -57 dBm
TX bitrate: 1200.9 Mbit/s
RX bitrate: 960.7–1080.6 Mbit/s
rx drop misc: 1 (cumulative, unchanged during sample)
```

В `logread` роутера не найдено deauth/disassoc или явной ошибки
hostapd/wireless.

### 11.3. Контрольный 127-секундный Wi-Fi запуск с телеметрией роутера

Маршрут:

```text
192.168.1.158 dev wlp114s0 src 192.168.1.153
```

Сессия:

```text
UUID: 9DBDB5A1-34EB-140F-F090-08E8297F5C3D
duration: 127.1 s
verdict: healthy
max encode_latency_ms: 4.4 ms
client_reported_losses: 0
video/audio drops: 0/0
```

Moonlight:

```text
host processing latency: 2.3/10.4/3.1 ms
frames dropped by network connection: 0.00%
frames dropped due to network jitter: 0.00%
average network latency: 2 ms
variance: 0 ms
```

За те же примерно 125 секунд на роутере:

```text
tx packets: 16,185,934 -> 16,996,796  (+810,862)
tx retries: 1,316,662 -> 1,365,337  (+48,675)
tx failed: 1,316,662 -> 1,365,337  (+48,675)
rx drop misc: 1 -> 1
signal: approximately -61…-64 dBm
```

Счётчики `tx retries` и `tx failed` на этом выводе нельзя интерпретировать
как 48,675 окончательно потерянных кадров: контрольный Moonlight-тест при
этом показал `0.00%` пропущенных видеокадров, а Vibepollo —
`client_reported_losses=0`.

### 11.4. Вывод

Подтверждено:

```text
1. Wi-Fi/AP выполняют retransmit: за 127 s было около 48.7k retry attempts.
2. В контрольном запуске retransmit успешно скрыли потери от Moonlight:
   frame drops = 0.00%.
3. В десятиминутном запуске Moonlight увидел 1.33% пропущенных frame numbers,
   но Vibepollo не увидел client losses, а router не показал deauth/disassoc.
4. Поэтому 1.33% — intermittent end-to-end frame loss/gap на клиенте,
   а не доказанные окончательные потери 802.11 на AP.
```

Наиболее вероятное объяснение разницы между десятью минутами и 127 секундами —
редкие Wi-Fi burst/retry эпизоды, которые в коротком контрольном запуске не
проявились. Текущих данных достаточно, чтобы исключить постоянную плохую
радиосвязь как единственную причину, но недостаточно, чтобы определить точный
момент каждого из 1.33% пропущенных кадров.

## 12. Prometheus correlation for subsequent tests

As of 2026-11-05, Prometheus `http://prometheus:9090` successfully returns
`up=1` for all four relevant scrape targets:

```text
openwrt-router   192.168.1.1:9100
windows-host     127.0.0.1:9182
nvidia-gpu       127.0.0.1:9835
vibepollo-exporter 127.0.0.1:9836
```

This materially improves test quality: router, Windows host, NVIDIA/NVENC and
Vibepollo samples share Prometheus timestamps and can be queried over the exact
same interval instead of being compared from unrelated final counters.

Recommended queries during a stream test:

```promql
# Vibepollo state and sender symptom
vibepollo_encode_latency_ms{job="vibepollo-exporter"}
vibepollo_encode_latency_max_ms{job="vibepollo-exporter"}
vibepollo_history_degraded{job="vibepollo-exporter"}
vibepollo_client_reported_losses{job="vibepollo-exporter"}
vibepollo_ref_invalidations{job="vibepollo-exporter"}

# NVIDIA/NVENC and thermal/power context
nvidia_smi_utilization_encoder_ratio{job="nvidia-gpu"}
nvidia_smi_encoder_stats_average_latency{job="nvidia-gpu"}
nvidia_smi_temperature_gpu{job="nvidia-gpu"}
nvidia_smi_power_draw_watts{job="nvidia-gpu"}

# Host process, GPU engine and network rates
rate(windows_process_cpu_time_seconds_total{job="windows-host",process="sunshine"}[15s])
rate(windows_gpu_engine_time_seconds_total{job="windows-host",process_id=~".+",engtype="VideoEncode"}[15s])
rate(windows_net_bytes_sent_total{job="windows-host"}[15s])

# Wi-Fi AP station and retransmission context
rate(wifi_station_tx_retries_total{job="openwrt-router",mac="2a:e9:f2:9e:b3:6d"}[15s])
rate(wifi_station_tx_failed_total{job="openwrt-router",mac="2a:e9:f2:9e:b3:6d"}[15s])
wifi_station_last_ack_signal_dbm{job="openwrt-router",mac="2a:e9:f2:9e:b3:6d"}
wifi_station_tx_bitrate_kbps{job="openwrt-router",mac="2a:e9:f2:9e:b3:6d"}
```

Use a range query with a 5–15 second step and the same test start/end time for
all expressions. The current exporter still does **not** expose per-frame
`encode_wait_ms`, `send_wait_ms` or queue depth, so Prometheus can correlate
symptoms and external pressure but cannot by itself prove which internal queue
blocked. A future `degraded` run should therefore be repeated with dynamic
content and `pacing=0` versus `pacing=85000`, while recording the Prometheus
range for both runs.

## 13. Wi-Fi A/B with Prometheus correlation (2026-11-05)

Route was explicitly kept on Wi-Fi for both runs:

```text
192.168.1.158 dev wlp114s0 src 192.168.1.153
SSID kolenka, 5 GHz, channel 36, 80 MHz
```

Common stream parameters:

```text
HEVC, 1920x1080@60, requested bitrate 100000 kbps
Moonlight: --display-mode windowed --no-vsync --no-quit-after
```

### A: pacing=0

```text
UUID: D9293790-F7DF-D907-6F87-E359BCFB408E
duration: 127.1 s
verdict: healthy
history samples: 64
dropped_samples: 0
max encode_latency_ms: 6.8 ms
client_reported_losses: 0
video/audio drops: 0/0
```

Moonlight final statistics:

```text
incoming/decoding/rendering: 59.35 FPS
host processing latency: 2.6/10.4/3.2 ms
network drops: 0.00%
jitter drops: 0.00%
network latency: 6 ms, variance 1 ms
```

Prometheus range `1793903101..1793903229`:

```text
encode latency: 0..6.8 ms, average 2.99 ms
actual_fps: 0..60.5, average 58.1
frame jitter: 0..3.42 ms, average 1.04 ms
host GPU encoder: 0..12.8%, average 10.0%
Sunshine CPU: 7.4..22.2%, average 9.7%
Wi-Fi TX retries: +17,343
Wi-Fi TX failed: +17,343
last ACK signal: -62..-53 dBm
TX bitrate: 961..1080 Mbit/s
```

### B: pacing=85000

```text
UUID: 076B85F7-C42F-BD0D-8400-8D12631561DB
duration: 126.8 s
verdict: healthy
history samples: 63
dropped_samples: 0
max encode_latency_ms: 4.0 ms
client_reported_losses: 0
video/audio drops: 0/0
```

Moonlight final statistics:

```text
incoming/decoding/rendering: 30.75 FPS
host processing latency: 2.6/12.0/3.2 ms
network drops: 0.00%
jitter drops: 0.00%
network latency: 3 ms, variance 0 ms
```

Prometheus range `1793903326..1793903454`:

```text
encode latency: 0..4.0 ms, average 1.10 ms
actual_fps: 0..60.5, average 30.0
frame jitter: 0..22.6 ms, average 17.2 ms
host GPU encoder: 0..11.8%, average 5.46%
Sunshine CPU: 4.1..21.1%, average 7.1%
Wi-Fi TX retries: +9,769
Wi-Fi TX failed: +9,769
last ACK signal: -64..-61 dBm
TX bitrate: 865..1080 Mbit/s
```

### Current interpretation

Both Wi-Fi runs were `healthy` by Vibepollo and had no reported client losses,
but `pacing=85000` clearly worsened delivery cadence: Moonlight received only
30.75 FPS and Vibepollo's `frame_interval_jitter_ms` averaged 17.2 ms versus
1.04 ms with pacing disabled. This is a pacing/queue-shape effect, not an
encoder overload: encode latency, CPU and NVENC utilization did not spike.
The test did not reproduce the earlier `degraded` verdict because the desktop
scene did not sustain the high dynamic bitrate needed for that failure mode.

After the A/B, configuration was restored and verified:

```text
pacing_max_bitrate_kbps=0
```

No active stream was left running.

## 14. Длительный Wi-Fi A/B на запущенном 60 FPS Desktop-видео

Дата: 2026-11-05, маршрут всё время оставался Wi-Fi:

```text
192.168.1.158 dev wlp114s0 src 192.168.1.153
HEVC 1920x1080@60, requested bitrate 100000 kbps
```

### A: 617 секунд, `pacing=0`

```text
UUID: F2FE6C44-F9F5-CA27-148F-6E8B89A9716B
duration: 616.7 s
verdict: degraded
Moonlight incoming/decoding/rendering: 59.29 FPS
Moonlight host processing latency: 2.5/35.0/3.4 ms
Moonlight network drops: 1.11%
Moonlight jitter drops: 0.00%
Moonlight network latency: 5 ms, variance 3 ms
```

Prometheus range `1793903810..1793904428`:

```text
encode_latency_ms: 2.7..34.2 ms, average 3.63 ms, p99 4.7 ms
actual_fps: average 59.52
frame jitter: average 0.046 ms, max 0.08 ms
client losses/video drops: 0/0
NVENC: average 11.44%
Sunshine CPU: average 8.48%
Wi-Fi TX retries: +122506
Wi-Fi TX failed: +122507
ACK signal: -73..-49 dBm
TX bitrate: 648..1200 Mbit/s
```

Был один Prometheus sample выше порога `16 ms`: около `34.2 ms`. Этого
достаточно для `degraded` по логике Vibepollo, даже при нулевых reported client
losses. Основной видимый клиентский ущерб — intermittent frame gaps на Wi-Fi.

### B: 617 секунд, `pacing=85000`

```text
UUID: F7BFADF8-70DE-012D-4706-0432E823F192
duration: 617.0 s
verdict: degraded
Moonlight incoming/decoding/rendering: 58.10 FPS
Moonlight host processing latency: 2.5/536.4/26.9 ms
Moonlight network drops: 1.80%
Moonlight jitter drops: 0.00%
Moonlight network latency: 2 ms, variance 0 ms
```

Prometheus range `1793904531..1793905149`:

```text
encode_latency_ms: 2.6..340.2 ms, average 23.13 ms, p99 305.4 ms
actual_fps: average 58.92
frame jitter: average 0.488 ms, max 1.44 ms
client losses/video drops: 0/0
NVENC: average 11.56%
Sunshine CPU: average 7.88%
Wi-Fi TX retries: +375920
Wi-Fi TX failed: +375920
ACK signal: -68..-62 dBm
TX bitrate: 865..1080 Mbit/s
```

Prometheus зарегистрировал 40 samples выше `16 ms`, с повторяющимися bursts до
`479.2 ms`. Это совпало с `degraded` и с ростом Moonlight host processing до
`536.4 ms`.

### Вывод длительного теста

На реальном длительном 60 FPS видео Wi-Fi-тест подтвердил оба механизма:

1. При `pacing=0` сеть дала около `1.11%` frame gaps и один редкий host latency
   spike до `34.2 ms`; этого уже достаточно для `degraded`.
2. При `pacing=85000` возникли систематические sender/queue bursts до
   `340.2 ms` (Moonlight до `536.4 ms`), а network drops выросли до `1.80%`.
3. CPU и NVENC не были перегружены, поэтому это не похоже на постоянное
   ограничение GPU/CPU.
4. `tx retries` выросли намного сильнее во втором тесте, но это не равно
   окончательной потере такого же числа пакетов: Vibepollo client losses остались
   нулевыми.

После тестов конфигурация восстановлена:

```text
pacing_max_bitrate_kbps=0
```

Проверка завершения:

```text
activeSessions: 0
RTSP sessions: 0
route: Wi-Fi (wlp114s0)
```

## 15. Что ещё нужно для окончательной локализации

Текущие данные уже локализуют проблему до связки `pacing/send queue ↔ Wi-Fi
retransmission`, но не доказывают, где именно появляется задержка. Для
окончательного разделения нужны три одновременно записанные временные шкалы.

### 15.1. Host-side per-frame instrumentation — обязательно

Добавить в Vibepollo/Sunshine или экспортировать из существующего pipeline:

```text
frame_id
capture_to_encode_ms
encode_wait_ms
encode_ms
packetize_ms
send_wait_ms
encode_queue_depth
send_queue_depth
pacing_lag_ms
socket_send_blocked_ms
frame_size_bytes
```

Достаточно sample на кадр либо агрегата каждые 100–500 ms. Главный критерий:

```text
encode_wait растёт → очередь перед encoder/capture
encode_ms растёт → encoder/GPU/capture processing
send_wait или pacing_lag растёт → pacing/send queue/socket
```

### 15.2. Синхронная packet/network capture

На ноутбуке нужны `tcpdump`/`tshark` с привилегией `CAP_NET_RAW` или root,
плюс Prometheus range за тот же интервал. Сейчас `tcpdump` и `tshark` установлены,
но у обычного пользователя capability не выдана; для capture потребуется
одноразовый root-доступ или заранее разрешённый `setcap`.

Минимально записывать:

```text
Wi-Fi interface: wlp114s0
host/client traffic to 192.168.1.158
UDP packet timestamps and sizes
socket/qdisc counters
router station retries/failed/ACK signal
```

Packet capture не покажет содержимое зашифрованного/видеопотока, но позволит
сопоставить burst/задержку доставки с `send_wait_ms` и Moonlight frame gaps.

### 15.3. Повторяемая матрица

Для каждого режима нужны минимум 3 повтора по 10 минут на той же динамичной
сцене:

```text
Wi-Fi + pacing=0
Wi-Fi + pacing=85000
Ethernet/Gigabit + pacing=0
Ethernet/Gigabit + pacing=85000
```

Каждый прогон должен сохранять UUID сессии, Moonlight итог, Prometheus range,
router counters и build/config. Текущий 100-Mbit USB Ethernet не является
чистым Gigabit-контролем.

### Критерий окончательной локализации

Причина считается доказанной, если во временной последовательности видно одно
из двух:

```text
encode_wait/encode_ms растёт первым, сеть стабильна
  => capture/encoder pipeline

send_wait/pacing_lag растёт первым, затем router retries и Moonlight gaps
  => pacing/send queue/network path
```

Сейчас уже доказано только более слабое утверждение: `pacing=85000` резко
увеличивает sender latency bursts на Wi-Fi, при этом CPU/NVENC не перегружены.
