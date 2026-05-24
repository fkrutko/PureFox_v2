# PureFox

**Сетевой аудио-плеер (endpoint) на Luckfox Pico Max / Ultra с поддержкой внешних клоков**

<img src="file:///C:/Users/admin/AppData/Roaming/marktext/images/2026-05-24-09-29-43-image.png" title="" alt="" width="519">

## О проекте

PureFox — прошивка для плат **Luckfox Pico** **Max** и **Ultra** на базе Rockchip RV1106, превращающая их в высококачественный сетевой аудио-транспорт с поддержкой:

##### Режим сетевого плеера

Выход **I2S** (внешнее тактирование EXT / встроенный синтезатор PLL)
Выход **USB**(UAC2)

##### Режим USB транспорта

USB(UAC2) в I2S

##### Поддерживаемые стандарты audio

PCM(44.1-768) и Native DSD(64–512)

## Характеристики

| Параметр    | Значение                  |
| ----------- | ------------------------- |
| Процессор   | Rockchip RV1106           |
| Ядро Linux  | 6.1                       |
| Потребление | 200–250 мА                |
| Память      | SPI NOR Flash или eMMC    |
| Питание     | 5V через USB Type-C       |
| Выходы      | I2S (EXT/PLL), USB (UAC2) |

## Прошивка

### Скачивание

Последняя версия прошивки доступна на:

- [MEGA](https://mega.nz/fm/v5YwHLBR) — официальный релиз
- [Яндекс.Диск](https://disk.yandex.ru/d/g8tOCR7uFATj5Q) — зеркало

### Установка (через USB)

1. Установите драйверы с [Luckfox Wiki](https://wiki.luckfox.com/)
2. Запустите [RV1106_Toolkit.exe](https://mega.nz/file/ndFiCATS#CG5lF0Nz7JWhmoyrECxeIQDIw3iS5Lv3PZq-MIzJa9c) от имени администратора, выберите **rv1106**
3. Зажмите кнопку **BOOT** на плате и подключите USB
4. Дождитесь появления **Maskrom** в программе
5. Выберите Download → USB → Search Path → папка с прошивкой
6. Отметьте все файлы и нажмите **Download**
7. После **Download done** подождите 1 минуту, затем отключите плату

### Веб-интерфейс

После загрузки устройство доступно по адресу `http://purefox/` или по IP.  
Логин SSH: `root`, пароль: `purefox`.

## Режимы работы

### I2S

- **EXT** — внешнее тактирование от мастер-клока
- **PLL** — синтезатор частоты RV1106

<img src="file:///C:/Users/admin/AppData/Roaming/marktext/images/2026-05-24-10-02-10-image.png" title="" alt="" width="389">



### USB (UAC2 Gadget)

- PureCore UAC2 — эмуляция USB Audio Class 2
- Поддержка PCM 2ch до 768kHz или PCM 8ch до 192kHz
- Поддержка DSD (DoP и Native через Alt-Setting 2)
- Режим **USB to I2S** — USB вход → I2S выход
- Для поддержеи native DSD и ASIO требуются проприетарные драйвера. Для тестирования предоставляются по запросу. Ведётся работа над собственными драйверами ASIO. 

### Переключение выходов

Через веб-интерфейс: тогл **I2S ↔ USB**

## Поддерживаемые плееры

- Roon Ready (RAAT)
- Spotify Connect (librespot)
- Tidal Connect (только в тестовой версии!)
- Qobuz Connect
- Squeezelite (LMS)
- MPD
- shairport-sync (AirPlay)
- NAA (HQPlayer)
- DLNA Bridge

## Ветки репозитория

| Ветка       | Платформа                 |
| ----------- | ------------------------- |
| `MAX_6.X`   | Luckfox Pico Max          |
| `ULTRA_6.X` | Luckfox Pico Ultra (eMMC) |

## Расположение выводов на примере LuckFox Pico MAX

![image](https://forum.puredsd.ru/uploads/default/optimized/2X/c/c4b521acfee5bceaba972793da2c3ec7d33bbdbd_2_533x500.jpeg)

BBB - это соответствие с выводами BeagleBone Black от смежного проекта [Pure_v2](https://github.com/ppy2/Pure_v2) 

## Контакты

- Форум: [PureDSD](https://forum.puredsd.ru/t/luckfox-pico-max-ultra-endpoint-s-vneshnimi-klokami-na-rockchip-rv1106/1172)
