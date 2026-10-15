<?php require_once 'config.php'; ?>
<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Audio System Control</title>
    <link rel="stylesheet" href="assets/css/style.css?v=<?php echo time(); ?>">
</head>
<body>
    <div class="container">
        <!-- Tabs: Player | Tweaks -->
        <div class="tabs seg" role="tablist">
            <button type="button" class="seg-option tab-btn active" role="tab" data-tab="player" data-lang="tab_player">Player</button>
            <button type="button" class="seg-option tab-btn" role="tab" data-tab="tweaks" data-lang="tab_tweaks">Tweaks</button>
        </div>

        <div class="tab-panel" id="tab-player">
        <!-- Player control buttons -->
        <button class="btn-custom warning" data-service="naa" data-process="naa_process">HQPlayer (NAA)</button>
        <button class="btn-custom primary" data-service="scream" data-process="scream">
            Scream
            <a href="scream_settings.php" class="settings-link" data-lang="settings">
                <img src="assets/img/settings.svg" class="settings-icon" alt="Settings">
            </a>
        </button>
        <button class="btn-custom primary" data-service="apscream" data-process="apscream">
            APScream (ASIO)
            <a href="apscream_settings.php" class="settings-link" data-lang="settings">
                <img src="assets/img/settings.svg" class="settings-icon" alt="Settings">
            </a>
        </button>
	<button class="btn-custom usb2i2s<?php echo file_exists('/etc/usb_to_i2s.state') ? ' active' : ''; ?>" id="usbto-i2s-btn">USB to I2S</button>
<!--	<button class="btn-custom usb2i2s" id="dlna-bridge-btn">
	    DLNA Bridge
	    <a href="#" class="settings-link" id="dlna-settings-btn" onclick="event.preventDefault(); event.stopPropagation(); openDlnaModal(); return false;">
	        <img src="assets/img/settings.svg" class="settings-icon" alt="Settings">
	    </a>
	</button>
-->
        <!-- ALSA toggle: USB | I2S -->
        <div class="alsa-toggle">
            <div class="alsa-left">
                <div class="toggle-switch">
                    <input type="checkbox" id="alsa-toggle" class="toggle-input">
                    <label for="alsa-toggle" class="toggle-label">
                        <span class="toggle-option left">USB</span>
                        <span class="toggle-slider"></span>
                        <span class="toggle-option right">I2S</span>
                    </label>
                </div>
                <a href="#" id="i2s-settings-link" class="i2s-settings-link" onclick="event.preventDefault(); openI2SModal(); return false;">
                    <img src="assets/img/settings.svg" class="settings-icon" alt="I2S Settings">
                </a>
            </div>
        </div>
        </div>

        <!-- Tweaks: the kernel and settings that do not change the audio data (lownoise.php, /opt/lownoise.sh) -->
        <div class="tab-panel" id="tab-tweaks" hidden>
            <h3 class="ln-warning" data-lang="ln_experimental">Experimental: use at your own risk.</h3>
            <!-- what works with the selected kernel, filled by loadKernelStatus() -->
            <p class="ln-kernel-info" id="ln-kernel-info" hidden></p>
            <p class="ln-intro" data-lang="ln_intro">These settings do not change the audio data. Compare them by ear, one at a time.</p>

            <!-- Linux kernel: 6.1 | 5.10 | 5.10 with the PureFox 1.6 driver (shown when the image has more than 6.1) -->
            <div class="ln-row" id="kernel-row" hidden>
                <div class="ln-text">
                    <div class="ln-title" data-lang="kernel_caption">Kernel</div>
                    <div class="ln-hint" data-lang="ln_kernel_hint">Writes the kernel to flash and reboots. Default: 6.1.</div>
                </div>
                <div class="seg">
                    <button type="button" class="seg-option kernel-option" data-kernel="6.1">6.1</button>
                    <button type="button" class="seg-option kernel-option" data-kernel="5.10">5.10</button>
                    <button type="button" class="seg-option kernel-option" data-kernel="1.6">5.10 (old drv)</button>
                </div>
            </div>

            <div class="ln-row" id="ln-cpu-row" hidden>
                <div class="ln-text">
                    <div class="ln-title" data-lang="ln_cpu">CPU frequency</div>
                    <div class="ln-hint"><span data-lang="ln_cpu_hint">Lower clock, less supply noise. Check DSD512 and 768 kHz for dropouts. Default: 1608 MHz on 5.10, 1104 MHz on 6.1.</span> <span id="ln-cpu-now"></span></div>
                </div>
                <select class="ln-select" id="ln-cpu" data-key="CPU_MAX_FREQ"></select>
            </div>

            <div class="ln-row">
                <div class="ln-text">
                    <div class="ln-title" data-lang="ln_drive">I2S pin drive</div>
                    <div class="ln-hint" data-lang="ln_drive_hint">Edge speed of SCLK, LRCK and the data outputs. Lower: less ringing and ground bounce; higher for long wires. Default: level 2.</div>
                </div>
                <select class="ln-select ln-drive" id="ln-drive" data-key="I2S_DRIVE"></select>
            </div>

            <div class="ln-row">
                <div class="ln-text">
                    <div class="ln-title" data-lang="ln_mclk_drive">MCLK pin drive</div>
                    <div class="ln-hint" data-lang="ln_mclk_drive_hint">Edge speed of the MCLK output. No effect in EXT mode, where MCLK is an input. Default: level 3.</div>
                </div>
                <select class="ln-select ln-drive" id="ln-mclk-drive" data-key="MCLK_DRIVE"></select>
            </div>

            <div class="ln-row">
                <div class="ln-text">
                    <div class="ln-title" data-lang="ln_dma">DMA periods per buffer</div>
                    <div class="ln-hint"><span data-lang="ln_dma_hint">Fewer, longer periods: fewer interrupts. Applies when playback starts. Default: 16.</span> <span id="ln-dma-reboot" data-lang="ln_dma_reboot" hidden>Reboot to enable.</span></div>
                </div>
                <div class="seg" id="ln-dma">
                    <button type="button" class="seg-option ln-option" data-key="DMA_PERIODS" data-value="16">16</button>
                    <button type="button" class="seg-option ln-option" data-key="DMA_PERIODS" data-value="8">8</button>
                    <button type="button" class="seg-option ln-option" data-key="DMA_PERIODS" data-value="4">4</button>
                    <button type="button" class="seg-option ln-option" data-key="DMA_PERIODS" data-value="2">2</button>
                </div>
            </div>

            <div class="ln-row">
                <div class="ln-text">
                    <div class="ln-title" data-lang="ln_scream">Scream resampling</div>
                    <div class="ln-hint" data-lang="ln_scream_hint">Off: a sample rate the output does not take fails instead of being resampled. Default: On.</div>
                </div>
                <div class="seg" id="ln-scream">
                    <button type="button" class="seg-option ln-option" data-key="SCREAM_RESAMPLE" data-value="0" data-lang="ln_off">Off</button>
                    <button type="button" class="seg-option ln-option" data-key="SCREAM_RESAMPLE" data-value="1" data-lang="ln_on">On</button>
                </div>
            </div>

            <!-- Back to the upstream firmware (/opt/to_ppy.sh) -->
            <div class="ln-row">
                <div class="ln-text">
                    <div class="ln-title" data-lang="ln_ppy">Back to ppy</div>
                    <div class="ln-hint" data-lang="ln_ppy_hint">Replaces this firmware with the latest upstream build of ppy2 and reboots. Settings return to its defaults. To come back, run the command from the README over SSH.</div>
                </div>
                <button type="button" class="ln-button" id="to-ppy" data-lang="ln_ppy_button">Back to ppy</button>
            </div>
        </div>

        <!-- Update and power management buttons -->
        <div class="power-controls">
            <button id="update-firmware" class="btn-custom danger firmware-btn">
                <div class="firmware-text">
                    <div class="firmware-title">PureFox v<?php echo VERSION; ?>f</div>
                    <div class="firmware-subtitle">for LuckFox Pico MAX</div>
                </div>
                <img src="assets/img/firmware.svg" class="settings-icon firmware-icon" alt="Firmware">
            </button>
            <div class="system-buttons">
                <button type="button" class="btn-custom system-btn" id="reboot-link">
                    <img src="assets/img/reboot.svg" class="settings-icon reboot-icon" alt="Reboot">
                </button>
                <button type="button" class="btn-custom system-btn" id="shutdown-link">
                    <img src="assets/img/shutdown.svg" class="settings-icon shutdown-icon" alt="Shutdown">
                </button>
            </div>
        </div>
    </div>

    <!-- DLNA settings modal -->
    <div id="dlna-modal" class="modal-overlay">
        <div class="modal-content">
            <h2>DLNA Bridge</h2>
            <div id="dlna-stream-info" style="font-size:0.85em;margin-bottom:8px;color:#aaa;"></div>
            <div id="dlna-renderer-list" style="margin-bottom:10px;"></div>
            <div style="margin-bottom:8px;">
                <input id="dlna-manual-ip"   placeholder="Renderer IP"   style="width:48%;margin-right:2%">
                <input id="dlna-manual-port"  placeholder="Port (e.g.52235)" style="width:48%" type="number">
            </div>
            <input id="dlna-manual-url" placeholder="/AVTransport/control" style="width:100%;margin-bottom:8px">
            <div style="display:flex;gap:8px;">
                <button id="dlna-discover-btn" class="btn-custom small">Discover</button>
                <button id="dlna-push-btn"     class="btn-custom small">Push</button>
                <button id="dlna-modal-close"  class="btn-custom small">Close</button>
            </div>
        </div>
    </div>

    <!-- Firmware update log modal -->
    <div id="update-log-modal" class="modal-overlay">
        <div class="modal-content">
            <h2 class="modal-header" data-lang="firmware_update">Обновление прошивки</h2>
            <pre id="update-log-content" class="modal-log-content"></pre>
            <button class="close-modal modal-close-btn" data-lang="close">Закрыть</button>
        </div>
    </div>

    <!-- Player switching spinner -->
    <div class="spinner-overlay">
        <div class="spinner-container">
            <div class="spinner"></div>
            <div class="spinner-text" data-lang="switching_player">Переключение плеера...</div>
        </div>
    </div>

    <!-- Custom confirm dialog -->
    <div id="custom-confirm" class="confirm-overlay">
        <div class="confirm-content">
            <div id="confirm-message" class="confirm-message"></div>
            <div class="confirm-buttons">
                <button id="confirm-yes" class="confirm-btn confirm-btn-yes">Да</button>
                <button id="confirm-no" class="confirm-btn confirm-btn-no">Отмена</button>
            </div>
        </div>
    </div>

    <!-- Custom alert dialog -->
    <div id="custom-alert" class="confirm-overlay">
        <div class="confirm-content">
            <div id="alert-message" class="confirm-message"></div>
            <div class="confirm-buttons">
                <button id="alert-ok" class="confirm-btn confirm-btn-yes">OK</button>
            </div>
        </div>
    </div>

    <!-- I2S Settings Modal -->
    <div id="i2s-modal" class="modal-overlay">
        <div class="modal-content i2s-modal-content">
            <div class="header">
                <h1 data-lang="i2s_title">I2S Settings</h1>
            </div>
             <form id="i2s-form" method="post" action="handle_i2s.php">
                <div class="i2s-group">
                    <div class="i2s-group-header">
                        <h3 data-lang="mode_title">Режим</h3>
                        <div class="toggle-switch-compact">
                            <input type="radio" name="mode" value="pll" id="modal-mode-pll" class="toggle-input-compact">
                            <input type="radio" name="mode" value="ext" id="modal-mode-ext" class="toggle-input-compact">
                            <label class="toggle-label-compact">
                                <div class="toggle-slider-compact"></div>
                                <span class="toggle-option-compact left" data-lang="pll_mode" onclick="document.getElementById('modal-mode-pll').click()">PLL</span>
                                <span class="toggle-option-compact right" data-lang="ext_mode" onclick="document.getElementById('modal-mode-ext').click()">EXT</span>
                            </label>
                        </div>
                    </div>
                </div>
                <div class="i2s-group">
                    <div class="i2s-submode-rows">
                        <div class="i2s-submode-row">
                            <button type="button" name="submode" value="std" class="btn-custom i2s-submode-btn" data-lang="std_mode">STD</button>
                            <button type="button" name="submode" value="8ch" class="btn-custom i2s-submode-btn" data-lang="8ch_mode">8CH</button>
                        </div>
                        <div class="i2s-submode-row">
                            <button type="button" name="submode" value="lr" class="btn-custom i2s-submode-btn" data-lang="lr_mode">L/R</button>
                            <button type="button" name="submode" value="plr" class="btn-custom i2s-submode-btn" data-lang="plr_mode">±L/±R</button>
                        </div>
                    </div>
                </div>
                <div class="i2s-group">
                    <div class="i2s-group-header">
                        <h3 data-lang="mclk_title">MCLK</h3>
                        <div class="toggle-switch-compact">
                            <input type="radio" name="mclk" value="512" id="modal-mclk-512" class="toggle-input-compact">
                            <input type="radio" name="mclk" value="1024" id="modal-mclk-1024" class="toggle-input-compact">
                            <label class="toggle-label-compact">
                                <div class="toggle-slider-compact"></div>
                                <span class="toggle-option-compact left" onclick="document.getElementById('modal-mclk-512').click()">512</span>
                                <span class="toggle-option-compact right" onclick="document.getElementById('modal-mclk-1024').click()">1024</span>
                            </label>
                        </div>
                    </div>
                </div>
                <div class="i2s-group">
                    <div class="i2s-group-header">
                        <h3 data-lang="pcm_swap_title">PCM Swap</h3>
                        <div class="toggle-switch-compact">
                            <input type="radio" name="pcm_swap" value="0" id="modal-pcm-normal" class="toggle-input-compact">
                            <input type="radio" name="pcm_swap" value="1" id="modal-pcm-swap" class="toggle-input-compact">
                            <label class="toggle-label-compact">
                                <div class="toggle-slider-compact"></div>
                                <span class="toggle-option-compact left" onclick="document.getElementById('modal-pcm-normal').click()">OFF</span>
                                <span class="toggle-option-compact right" onclick="document.getElementById('modal-pcm-swap').click()">ON</span>
                            </label>
                        </div>
                    </div>
                </div>
                <div class="i2s-group">
                    <div class="i2s-group-header">
                        <h3 data-lang="dsd_swap_title">DSD Swap</h3>
                        <div class="toggle-switch-compact">
                            <input type="radio" name="dsd_swap" value="0" id="modal-dsd-normal" class="toggle-input-compact">
                            <input type="radio" name="dsd_swap" value="1" id="modal-dsd-swap" class="toggle-input-compact">
                            <label class="toggle-label-compact">
                                <div class="toggle-slider-compact"></div>
                                <span class="toggle-option-compact left" onclick="document.getElementById('modal-dsd-normal').click()">OFF</span>
                                <span class="toggle-option-compact right" onclick="document.getElementById('modal-dsd-swap').click()">ON</span>
                            </label>
                        </div>
                    </div>
                </div>
                <div class="i2s-group">
                    <div class="i2s-group-header">
                        <h3 data-lang="freq_swap_title">44/48 Swap</h3>
                        <div class="toggle-switch-compact">
                            <input type="radio" name="freq_swap" value="0" id="modal-freq-normal" class="toggle-input-compact">
                            <input type="radio" name="freq_swap" value="1" id="modal-freq-swap" class="toggle-input-compact">
                            <label class="toggle-label-compact">
                                <div class="toggle-slider-compact"></div>
                                <span class="toggle-option-compact left" onclick="document.getElementById('modal-freq-normal').click()">OFF</span>
                                <span class="toggle-option-compact right" onclick="document.getElementById('modal-freq-swap').click()">ON</span>
                            </label>
                        </div>
                    </div>
                </div>
                <div class="i2s-group" style="display:none;">
                    <div class="i2s-group-header">
                        <h3 data-lang="leftjust_title">LeftJust</h3>
                        <div class="toggle-switch-compact">
                            <input type="radio" name="leftjust" value="0" id="modal-leftjust-off" class="toggle-input-compact">
                            <input type="radio" name="leftjust" value="1" id="modal-leftjust-on" class="toggle-input-compact">
                            <label class="toggle-label-compact">
                                <div class="toggle-slider-compact"></div>
                                <span class="toggle-option-compact left" onclick="document.getElementById('modal-leftjust-off').click()">OFF</span>
                                <span class="toggle-option-compact right" onclick="document.getElementById('modal-leftjust-on').click()">ON</span>
                            </label>
                        </div>
                    </div>
                </div>
                <div class="i2s-warning">
                    <span class="i2s-pulse" data-lang="warning_attention">Внимание!</span>
                    <span data-lang="warning_text1">Выход MCLK в режимах PLL и EXT имеет разные настройки (OUTPUT/INPUT).</span> <span data-lang="warning_text2">После изменения настроек I2S необходима перезагрузка системы для вступления в силу.</span>
                </div>
                <div class="modal-bottom-buttons">
                    <a href="#" onclick="closeI2SModal(); return false;" class="close-link">
                        <img src="assets/img/home.svg" class="settings-icon close-icon" alt="">
                    </a>
                    <a href="#" onclick="confirmRebootI2S(event)" class="reboot-link">
                        <img src="assets/img/reboot.svg" class="settings-icon reboot-icon" alt="">
                    </a>
                </div>
            </form>
        </div>
    </div>

    <!-- JavaScript includes -->
    <script src="assets/js/jquery-3.7.1.min.js"></script>
    <script src="assets/js/app.js?v=<?php echo VERSION; ?>&t=<?php echo time(); ?>"></script>
<script>
document.addEventListener('DOMContentLoaded', function() {
  setTimeout(function() {
    $.ajax({
      url: 'usb_to_i2s.php',
      method: 'POST',
      dataType: 'json',
      data: { action: 'status' },
      success: function(r) {
        if (r.enabled) $('#usbto-i2s-btn').addClass('active');
      }
    });
  }, 1000);
});
</script>
</body>
</html>
