# PureFox

**Network audio endpoint on Luckfox Pico Max / Ultra with external clock support**

> This is a fork of [ppy2/PureFox_v2](https://github.com/ppy2/PureFox_v2) (`MAX_6.X` branch). It keeps only the NAA and Scream players and makes the output bit-perfect. See [Changes in this fork](#changes-in-this-fork).


## About

PureFox is a firmware for **Luckfox Pico Max** and **Ultra** boards based on Rockchip RV1106, turning them into a high-quality network audio transport with support for:

#### Network Player Mode

- **I2S** output (external clocking EXT / internal PLL synthesizer)
- **USB** output (UAC2)

#### USB Transport Mode

- USB (UAC2) to I2S

#### Supported Audio Standards

- PCM 2ch up to 768 kHz or PCM 8ch up to 192 kHz
- Native DSD (64–512 over USB; up to DSD1024 in I2S mode with the 1024x MCLK multiplier)

## Changes in this fork

Compared with the upstream [ppy2/PureFox_v2](https://github.com/ppy2/PureFox_v2):

### Audio

- **Bit-perfect output.** The I2S driver no longer has a software volume control, so PCM reaches the DAC unaltered. Mute is kept (GPIO mute, auto-mute, the "PCM Playback Switch" control). The volume slider in the web interface and the rotary volume encoder support are removed.
- **DSD1024 over I2S** with the 1024x MCLK multiplier (45.1584 / 49.152 MHz). USB to I2S mode stays at DSD512.
- **DSD fast path** in the I2S driver: the DSD word swap is done in a cached buffer, so the uncached DMA buffer is written only once per sample.
- **Kernel choice: 6.1, 5.10, or 5.10 with the old driver.** The "Kernel 6.1 | 5.10 | 5.10 (old drv)" selector on the main page writes the selected kernel to the boot partition and reboots the board.
  - **5.10** is the PureFox 1.x kernel (Rockchip BSP 5.10.160 with the PureFox 1.6 kernel config) running this fork's I2S driver: bit-perfect, DSD1024, the same I2S settings. So 6.1 and 5.10 differ only in the kernel.
  - **5.10 (old drv)** is the same 5.10 kernel with the I2S driver and device trees of PureFox 1.6 (including its I2S clock setup), as in firmware 1.6. DSD512 max over I2S; PCM passes unaltered at volume 100 (the web interface no longer changes it).
  - USB to I2S mode does not work with kernel 5.10, with either driver (the USB port is host only). The choice is kept by the online update. From the shell: `/opt/kernel.sh 6.1`, `5.10` or `1.6` (5.10 with the old driver), then reboot.

### Players

- **Only NAA (HQPlayer) and Scream remain.** Roon Ready, Squeezelite, AirPlay (shairport-sync), MPD/upmpdcli, APlayer, APrender, Spotify Connect, Qobuz Connect, Tidal Connect and CelMusper are removed, together with the libraries only they used. The root filesystem image shrank from about 39 MB to 20 MB.
- **Scream instead of APScream.** The closed-source APScream receiver is replaced by the open-source [Scream](https://github.com/duncanthrax/scream) 3.6 receiver (ALSA output). It needs the standard Scream driver on Windows; APScream's ASIO sender in AP or TCP mode is not supported. The settings page sets the mode (multicast/unicast), multicast group, port, network interface, latency and ALSA device.
- **NAA**: newer `networkaudiod`; the name shown in HQPlayer is unique per board (hostname plus the last six hex digits of the MAC address).
- **The selected player survives a power cut**: the choice is written to flash as soon as it is made.

### Firmware and updates

- **Releases on GitHub.** Firmware builds are published as [GitHub releases](https://github.com/fkrutko/PureFox_v2/releases) of this repository, numbered `2.8.<commit count>` (e.g. 2.8.166).
- **Online update from GitHub.** The version button in the web interface downloads the latest release over HTTPS with certificate verification (curl), checks its SHA-256 checksums and installs it over the running system, keeping user settings (output, I2S settings, Scream settings, password, …). The kernel is flashed only when it changed. The update no longer pulls files from luckfox.puredsd.ru.

### Building

- `./release.sh` builds the current commit locally and publishes it as the latest GitHub release (`gh` required). `./build.sh` only builds.
- Builds on hosts with GCC 15 (e.g. Fedora 43); Buildroot uses ccache.

## Specifications

| Parameter          | Value                     |
| ------------------ | ------------------------- |
| Processor          | Rockchip RV1106           |
| Linux Kernel       | 6.1 or 5.10 (selectable)  |
| Power Consumption  | 80–120 mA                |
| Storage            | SPI NOR Flash or eMMC     |
| Power Supply       | 5V via USB Type-C         |
| Outputs            | I2S (EXT/PLL), USB (UAC2) |

## Firmware

### Download

The latest firmware of this fork is on the [Releases page](https://github.com/fkrutko/PureFox_v2/releases/latest): `PureFox-<version>.zip` is the image for flashing over USB.

Upstream firmware (with all players): [MEGA](https://mega.nz/folder/jhJzXZKJ#Y6YbHsS9xCfJL4TKOVeQ4Q), [Yandex.Disk](https://disk.yandex.ru/d/g8tOCR7uFATj5Q).

### Installation (via USB)

1. Install drivers from [Luckfox Wiki](https://wiki.luckfox.com/Luckfox-Pico-RV1106/Downloads)
2. Run [RV1106_Toolkit.exe](https://mega.nz/file/ndFiCATS#CG5lF0Nz7JWhmoyrECxeIQDIw3iS5Lv3PZq-MIzJa9c) as Administrator, select **rv1106**
3. Hold the **BOOT** button on the board and connect USB
4. Wait for **Maskrom** to appear in the program
5. Unzip the firmware, then select Download → USB → Search Path → firmware folder
6. Check all files and click **Download**
7. After **Download done**, wait 1 minute, then disconnect the board

### Web Interface

Once booted, the device is available at `http://purefox/` or by IP. If the home DNS does not resolve short names, use the mDNS address `http://purefox.local/`.
The web interface supports automatic language switching (English, Russian, Chinese, German, French).  
Unused player buttons can be hidden by swiping left.  
The version button at the bottom installs the latest release from GitHub (online update).  
The "Kernel 6.1 | 5.10 | 5.10 (old drv)" selector chooses the kernel; the board reboots.

SSH access is enabled: login `root`, password: `purefox`.

## Operating Modes

### I2S

- **EXT** — external master clock
- **PLL** — RV1106 frequency synthesizer. The quality of the internal PLL is surprisingly high. According to numerous subjective tests by audio experts, the internal PLL sound quality rivals that of expensive external clock generators.

<img title="" src="images/2026-05-24-10-02-10-image.png" alt="" width="389">

The I2S menu settings are configured in the web interface (the "I2S Settings" page) and let you adapt the I2S output to your DAC:

- **Mode** — `PLL` / `EXT`:
  - `PLL` — clocked by the internal RV1106 synthesizer (MCLK acts as **OUTPUT**).
  - `EXT` — external master clock (MCLK acts as **INPUT**).
- **Output Mode** — `STD` / `8CH` / `L/R` / `±L/±R`:
  - `STD` — standard 2-channel output.
  - `8CH` — 8-channel mode (PCM up to 192 kHz).
  - `L/R` — dual-mono mode.
  - `±L/±R` — dual-mono with balanced output.
- **MCLK** — `512` / `1024` (master clock multiplication factor).
- **PCM Swap** — `OFF` / `ON` — invert channel order for PCM.
- **DSD Swap** — `OFF` / `ON` — invert physical channels for DSD.
- **44/48 Swap** — `OFF` / `ON` — reverse frequency domain order (44.1 / 48 kHz).

> **Warning:** the MCLK output has different settings in PLL and EXT modes (OUTPUT/INPUT), and any change to the I2S settings takes effect only after a **device reboot**.


### USB (UAC2 Gadget)

- PureCore UAC2 — USB Audio Class 2 emulation
- PCM 2ch up to 768 kHz
- DSD support (DoP and Native via Alt-Setting 2)
- **USB to I2S** mode — USB input → I2S output
- Proprietary drivers required for Native DSD and ASIO. Available for testing upon request. Custom ASIO drivers are in development.

### Output Switching

Via web interface: **I2S ↔ USB** toggle

## Supported Players

- NAA (HQPlayer)
- Scream (Windows network sound card)

## Repository Branches

| Branch       | Platform                  |
| ------------ | ------------------------- |
| `MAX_6.X`    | Luckfox Pico Max          |
| `ULTRA_6.X`  | Luckfox Pico Ultra (eMMC) |

## Pinout (LuckFox Pico MAX example)

![image](https://forum.puredsd.ru/uploads/default/optimized/2X/c/c4b521acfee5bceaba972793da2c3ec7d33bbdbd_2_533x500.jpeg)

BBB — corresponds to BeagleBone Black pins from the related project [Pure_v2](https://github.com/ppy2/Pure_v2)

## Contacts

- Forum: [PureDSD](https://forum.puredsd.ru/t/luckfox-pico-max-ultra-endpoint-s-vneshnimi-klokami-na-rockchip-rv1106/1172)
