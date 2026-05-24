# PureFox

**Сетевой аудио-плеер (endpoint) на Luckfox Pico Max / Ultra с поддержкой внешних клоков**

## О проекте

PureFox — прошивка для плат Luckfox Pico Max и Ultra на базе Rockchip RV1106, превращающая их в высококачественный сетевой аудио-транспорт с поддержкой:

- Выхода **I2S** (внешнее тактирование EXT / встроенный синтезатор PLL)
- Выхода **USB** (UAC2 Gadget)
- **Native DSD** (DSD64–DSD512)
- Переключения режимов через **веб-интерфейс**

## Характеристики

| Параметр | Значение |
|----------|---------|
| Процессор | Rockchip RV1106 |
| Ядро Linux | 6.1 |
| Потребление | 200–250 мА |
| Память | SPI NOR Flash (карта SD не требуется) |
| Питание | 5V через USB Type-C |
| Поддержка DSD | DSD64–DSD512 (Native) |
| Выходы | I2S (EXT/PLL), USB (UAC2) |

## Прошивка

### Скачивание

Последняя версия прошивки доступна на:
- [MEGA](https://mega.nz/folder/...) — официальный релиз
- [Яндекс.Диск](https://disk.yandex.ru/...) — зеркало

### Установка (через USB)

1. Установите драйверы с [Luckfox Wiki](https://wiki.luckfox.com/)
2. Запустите `RV1106_Toolkit.exe` от имени администратора, выберите **rv1106**
3. Зажмите кнопку **BOOT** на плате и подключите USB
4. Дождитесь появления **Maskrom** в программе
5. Выберите Download → USB → Search Path → папка с прошивкой
6. Отметьте все файлы и нажмите **Download**
7. После `Download done` подождите 1 минуту, затем отключите плату

### Веб-интерфейс

После загрузки устройство доступно по адресу `http://luckfox/` или по IP.  
Логин SSH: `root`, пароль: `purefox`.

## Режимы работы

### I2S

- **EXT** — внешнее тактирование от мастер-клока
- **PLL** — синтезатор частоты RV1106
- Поддержка субрежимов: STD, 1024fs, 512fs

### USB (UAC2 Gadget)

- PureCore UAC2 — эмуляция USB Audio Class 2
- Поддержка PCM до 768kHz
- Поддержка DSD (DoP и Native через Alt-Setting 2)
- Режим **USB to I2S** — USB вход → I2S выход

### Переключение выходов

Через веб-интерфейс: тогл **I2S ↔ USB**

## Поддерживаемые плееры

- Roon Ready (RAAT)
- Spotify Connect (librespot)
- Tidal Connect
- Qobuz Connect
- Squeezelite (LMS)
- MPD
- shairport-sync (AirPlay)
- NAA (HQPlayer)
- DLNA Bridge

## Ветки репозитория

| Ветка | Платформа |
|-------|-----------|
| `MAX_6.X` | Luckfox Pico Max |
| `ULTRA_6.X` | Luckfox Pico Ultra (eMMC) |

## Контакты

- Форум: [PureDSD](https://forum.puredsd.ru/t/luckfox-pico-max-ultra-endpoint-s-vneshnimi-klokami-na-rockchip-rv1106/1172)
- Авторы: **aleev**, **ppy**
- Тестировщики: [список на форуме](https://forum.puredsd.ru/t/luckfox-pico-max-ultra-endpoint-s-vneshnimi-klokami-na-rockchip-rv1106/1172)

## Благодарности

Павлу (автор идеи), @kvnik (vegalab.ru), Марату (@Cu6apum), команде Rockchip.

---

**Версия прошивки**: 2.x (альфа)  
**Статус**: активная разработка
