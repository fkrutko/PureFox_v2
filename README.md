# PureFox

**Сетевой аудио-плеер (endpoint) на Luckfox Pico Max / Ultra с поддержкой внешних клоков**

<img title="" src="images/2026-05-24-09-29-43-image.png" alt="" width="519">

## О проекте

PureFox — прошивка для плат **Luckfox Pico** **Max** и **Ultra** на базе Rockchip RV1106, превращающая их в высококачественный сетевой аудио-транспорт с поддержкой:

#### Режим сетевого плеера

- Выход **I2S** (внешнее тактирование EXT / встроенный синтезатор PLL)

- Выход **USB**(UAC2)

#### Режим USB транспорта

- USB(UAC2) в I2S

#### Поддерживаемые стандарты audio

- Поддержка PCM 2ch до 768kHz или PCM 8ch до 192kHz

- Поддержка native DSD (64-512)

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

- [MEGA](https://mega.nz/folder/jhJzXZKJ#Y6YbHsS9xCfJL4TKOVeQ4Q) — официальный релиз
- [Яндекс.Диск](https://disk.yandex.ru/d/g8tOCR7uFATj5Q) — зеркало

### Установка (через USB)

1. Установите драйверы с [Luckfox Wiki](https://wiki.luckfox.com/Luckfox-Pico-RV1106/Downloads)
2. Запустите [RV1106_Toolkit.exe](https://mega.nz/file/ndFiCATS#CG5lF0Nz7JWhmoyrECxeIQDIw3iS5Lv3PZq-MIzJa9c) от имени администратора, выберите **rv1106**
3. Зажмите кнопку **BOOT** на плате и подключите USB
4. Дождитесь появления **Maskrom** в программе
5. Выберите Download → USB → Search Path → папка с прошивкой
6. Отметьте все файлы и нажмите **Download**
7. После **Download done** подождите 1 минуту, затем отключите плату

### Веб-интерфейс

После загрузки устройство доступно по адресу `http://purefox/` или по IP.  
Веб интерфейс поддерживает автоматическое переключение языков (английский, русский, китайский, немецкий, французский).
Лишние кнопки плееров можно скрывать свайпом влево.
Нижняя кнопка с номером версии для онлайн обновления прошивки.

Разрешён вход по SSH
логин `root`, пароль: `purefox`.

## Режимы работы

### I2S

- **EXT** — внешнее тактирование от мастер-клока
- **PLL** — синтезатор частоты RV1106. Качество внутреннего PLL на удивление очень высокое. По многим субъективным тестам аудио экспертов звучание внутреннего PLL не уступает звучанию дорогих внешних генераторов.

<img title="" src="images/2026-05-24-10-02-10-image.png" alt="" width="389">

### USB (UAC2 Gadget)

- PureCore UAC2 — эмуляция USB Audio Class 2
- Поддержка PCM 2ch до 768kHz
- Поддержка DSD (DoP и Native через Alt-Setting 2)
- Режим **USB to I2S** — USB вход → I2S выход
- Для поддержки native DSD и ASIO требуются проприетарные драйвера. Для тестирования предоставляются по запросу. Ведётся работа над собственными драйверами ASIO.  

### Переключение выходов

Через веб-интерфейс: тогл **I2S ↔ USB**

## Поддерживаемые плееры

- NAA (HQPlayer)
- Roon Ready (RAAT)
- Squeezelite (LMS)
- shairport-sync (AirPlay)
- MPD (UPmP)
- APlayer (только веб радио)
- APrender (UPnP)
- APScream (альтернатива Diretta)
- Spotify Connect (librespot)
- Qobuz Connect
- Tidal Connect (только в тестовой версии!)
  
  

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
