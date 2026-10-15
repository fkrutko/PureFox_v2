$(document).ready(function () {
    // Global variables
    let lastKnownStatus = null;
    let isServiceSwitching = false; // Flag to block updates during switching
    let serviceSwitchWatchdog = null;
    let serviceStatusProbeTimer = null;
    let pendingService = null;
    let isAlsaSwitching = false; // Flag to block ALSA updates during switching
    let statusInterval = null;
    let statusEvents = null;
    let statusEventsConnected = false;
    let statusRecoveryTimer = null;
    let currentBootId = null;
    let rebootRecoveryActive = false;
    let statusRequestSequence = 0;
    let isDlnaBridgeActive = false; // true when DLNA bridge is enabled
    const DEBUG_UI = new URLSearchParams(window.location.search).get('debug') === '1';
    const debugLog = (...args) => { if (DEBUG_UI) console.log(...args); };
    const debugWarn = (...args) => { if (DEBUG_UI) console.warn(...args); };

    function resetServiceSwitchFlag() {
        isServiceSwitching = false;
        pendingService = null;
        if (serviceSwitchWatchdog) {
            clearTimeout(serviceSwitchWatchdog);
            serviceSwitchWatchdog = null;
        }
        if (serviceStatusProbeTimer) {
            clearTimeout(serviceStatusProbeTimer);
            serviceStatusProbeTimer = null;
        }
        hideSpinner();
    }
    
    // Universal interface update function
    function updateInterfaceFromStatus(data) {
        // Update active service ONLY if switching is not in progress AND not in USB-to-I2S
        if (data.active_service !== undefined && !$('#usbto-i2s-btn').hasClass('active')) {
            // During a switch, ignore the stopped player's final status but
            // accept the requested player as soon as status_monitor reports it.
            if (isServiceSwitching && data.active_service === pendingService) {
                resetServiceSwitchFlag();
            }
            if (!isServiceSwitching) {
                $('button[data-service]').removeClass('active');
                if (data.active_service) {
                    $(`button[data-service="${data.active_service}"]`).addClass('active');
                }
            }
        }

        // Update ALSA state ONLY if not switching AND USBtoI2S mode is not enabled
        const usbToI2sEnabled = $('#usbto-i2s-btn').hasClass('active');
        if (data.alsa_state !== undefined && !isAlsaSwitching && !usbToI2sEnabled) {
            updateAlsaUI(data.alsa_state);
        }
    }
    let previousActiveService = null;


    // Language dictionaries (FULLY PRESERVED!)
    window.translations = {
        'ru': {
            'Output': 'Выход:',
            'i2s_settings': 'Настройки I2S',
            'update_firmware': 'Обновить прошивку',
            'update_firmware_short': 'Обновить',
            'firmware_update': 'Обновление прошивки',
            'close': 'Закрыть',
            'update_start': 'Запуск обновления...',
            'update_finish': 'Обновление завершено. Перезагрузите LuckFox',
            'update_error': 'Ошибка при обновлении.',
            'confirm_update': 'Вы уверены, что хотите обновить прошивку?',
            'alsa_error': 'Ошибка при переключении ALSA',
            'service_error': 'Ошибка при переключении сервиса',
            'service_switch_in_progress': 'Переключение плеера ещё не завершено. Подождите.',
            'settings': '',
            'switching_player': 'Переключение плеера...',
            'switching_output': 'Переключение выхода...',
            'switching_input': 'Переключение входа...',
            'usb_dac_missing': 'USB ЦАП не обнаружен.<br>Пожалуйста, подключите USB ЦАП',
            'confirm_reboot': 'Вы уверены, что хотите перезагрузить систему?',
            'confirm_shutdown': 'Вы уверены, что хотите выключить систему?',
            'shutdown_complete': 'Система выключена. Можете отключить питание.',
            'i2s_title': 'I2S Настройки',
            'back_button': '← Назад',
            'mode_title': 'Режим',
            'pll_mode': 'PLL',
            'ext_mode': 'EXT',
            'submode_title': 'Вариант выхода',
            'lr_mode': 'L/R',
            'plr_mode': '±L/±R',
            'std_mode': 'STD',
            'l_mode': 'L',
            'r_mode': 'R',
            'plusl_mode': '±L',
            'plusr_mode': '±R',
            '8ch_mode': '8CH',
            'mclk_title': 'MCLK',
            'warning_attention': 'Внимание!',
            'warning_text1': 'Выход MCLK в режимах PLL и EXT имеет разные настройки (OUTPUT/INPUT).',
            'warning_text2': 'После изменения настроек I2S необходима перезагрузка системы для вступления в силу.',
            'yes_btn': 'Да',
            'cancel_btn': 'Отмена',
            'apply_reboot': 'Применить и перезагрузить',
            'pcm_swap_title': 'PCM Swap',
            'dsd_swap_title': 'DSD Swap',
            'freq_swap_title': '44/48 Swap',
            'leftjust_title': 'LeftJust',
            'confirm_kernel': 'Перезагрузить LuckFox с ядром Linux %s?<br>Ядро будет записано во флеш-память, не отключайте питание.',
            'switching_kernel': 'Переключение ядра и перезагрузка...',
            'kernel_error': 'Не удалось переключить ядро',
            'kernel_usb_to_i2s': 'USB to I2S работает только с ядром 6.1. Сначала выключите USB to I2S.',
            'usb_to_i2s_kernel': 'USB to I2S работает только с ядром Linux 6.1',
            'kernel_caption': 'Ядро',
            'kernel_pf16': '5.10 с драйвером I2S из PureFox 1.6',
            'tab_player': 'Плеер',
            'tab_tweaks': 'Твики',
            'ln_experimental': 'Экспериментальный раздел: используйте на свой страх и риск.',
            'kernel_info_6.1': 'Ядро 6.1. Работает: DSD до DSD1024 по I2S, USB to I2S. Процессор по умолчанию на 1104 МГц.',
            'kernel_info_5.10': 'Ядро 5.10. Работает: DSD до DSD1024 по I2S, вывод на USB ЦАП. Не работает: USB to I2S (USB-порт только хост). Процессор по умолчанию на 1608 МГц.',
            'kernel_info_1.6': 'Ядро 5.10 со старым драйвером I2S из PureFox 1.6. Работает: DSD до DSD512 по I2S, вывод на USB ЦАП. Не работает: DSD1024, USB to I2S (USB-порт только хост). Процессор по умолчанию на 1608 МГц.',
            'ln_intro': 'Эти настройки не меняют аудиоданные. Сравнивайте на слух, по одной.',
            'ln_kernel_hint': 'Ядро записывается во флеш-память, затем перезагрузка. По умолчанию: 6.1.',
            'ln_cpu': 'Частота процессора',
            'ln_cpu_hint': 'Ниже частота — меньше помех по питанию. Проверьте DSD512 и 768 кГц на выпадения. По умолчанию: 1608 МГц на 5.10, 1104 МГц на 6.1.',
            'ln_cpu_now': 'Сейчас %s МГц.',
            'ln_default': 'По умолчанию',
            'ln_level': 'Уровень %s',
            'ln_drive': 'Сила выходов I2S',
            'ln_drive_hint': 'Крутизна фронтов SCLK, LRCK и выходов данных. Ниже — меньше звона и помех по земле; выше — для длинных проводов. По умолчанию: уровень 2.',
            'ln_mclk_drive': 'Сила выхода MCLK',
            'ln_mclk_drive_hint': 'Крутизна фронтов выхода MCLK. В режиме EXT не действует: там MCLK — вход. По умолчанию: уровень 3.',
            'ln_dma': 'Периодов DMA на буфер',
            'ln_dma_hint': 'Меньше периодов, они длиннее — меньше прерываний. Действует со следующего запуска воспроизведения. По умолчанию: 16.',
            'ln_dma_reboot': 'Перезагрузите плату, чтобы включить.',
            'ln_scream': 'Ресемплинг Scream',
            'ln_scream_hint': 'Выкл: частота, которую выход не поддерживает, не пересчитывается, а вызывает ошибку. По умолчанию: Вкл.',
            'ln_off': 'Выкл',
            'ln_on': 'Вкл',
            'ln_error': 'Не удалось применить настройку'
        },
        'en': {
            'alsa_output': 'ALSA Output:',
            'i2s_settings': 'I2S Settings',
            'update_firmware': 'Update Firmware',
            'update_firmware_short': 'Update',
            'firmware_update': 'Firmware Update',
            'close': 'Close',
            'update_start': 'Starting update...',
            'update_finish': 'Update completed. Please reboot LuckFox',
            'update_error': 'Update error.',
            'confirm_update': 'Are you sure you want to update the firmware?',
            'alsa_error': 'Error switching ALSA',
            'service_error': 'Error switching service',
            'service_switch_in_progress': 'Player switching is still in progress. Please wait.',
            'settings': '',
            'switching_player': 'Switching player...',
            'switching_output': 'Switching output...',
            'switching_input': 'Switching input...',
            'usb_dac_missing': 'USB DAC not detected.<br>Please connect USB DAC',
            'confirm_reboot': 'Are you sure you want to reboot the system?',
            'confirm_shutdown': 'Are you sure you want to shutdown the system?',
            'shutdown_complete': 'System has been shut down. You can disconnect power.',
            'i2s_title': 'I2S Settings',
            'back_button': '← Back',
            'mode_title': 'Mode',
            'pll_mode': 'PLL',
            'ext_mode': 'EXT',
            'submode_title': 'Output Mode',
            'lr_mode': 'L/R',
            'plr_mode': '±L/±R',
            'std_mode': 'STD',
            'l_mode': 'L',
            'r_mode': 'R',
            'plusl_mode': '±L',
            'plusr_mode': '±R',
            '8ch_mode': '8CH',
            'mclk_title': 'MCLK',
            'warning_attention': 'Warning!',
            'warning_text1': 'MCLK output has different settings in PLL and EXT modes (OUTPUT/INPUT).',
            'warning_text2': 'System reboot is required after changing I2S settings to apply them.',
            'yes_btn': 'Yes',
            'cancel_btn': 'Cancel',
            'apply_reboot': 'Apply & Reboot',
            'pcm_swap_title': 'PCM Swap',
            'dsd_swap_title': 'DSD Swap',
            'freq_swap_title': '44/48 Swap',
            'leftjust_title': 'LeftJust',
            'confirm_kernel': 'Reboot LuckFox with Linux kernel %s?<br>The kernel is written to flash, do not power off.',
            'switching_kernel': 'Switching kernel and rebooting...',
            'kernel_error': 'Kernel switch failed',
            'kernel_usb_to_i2s': 'USB to I2S works only with kernel 6.1. Turn USB to I2S off first.',
            'usb_to_i2s_kernel': 'USB to I2S works only with Linux kernel 6.1',
            'kernel_caption': 'Kernel',
            'kernel_pf16': '5.10 with the PureFox 1.6 I2S driver',
            'tab_player': 'Player',
            'tab_tweaks': 'Tweaks',
            'ln_experimental': 'Experimental: use at your own risk.',
            'kernel_info_6.1': 'Kernel 6.1. Works: DSD up to DSD1024 over I2S, USB to I2S. The CPU runs at 1104 MHz by default.',
            'kernel_info_5.10': 'Kernel 5.10. Works: DSD up to DSD1024 over I2S, output to a USB DAC. Does not work: USB to I2S (the USB port is host only). The CPU runs at 1608 MHz by default.',
            'kernel_info_1.6': 'Kernel 5.10 with the old PureFox 1.6 I2S driver. Works: DSD up to DSD512 over I2S, output to a USB DAC. Does not work: DSD1024, USB to I2S (the USB port is host only). The CPU runs at 1608 MHz by default.',
            'ln_intro': 'These settings do not change the audio data. Compare them by ear, one at a time.',
            'ln_kernel_hint': 'Writes the kernel to flash and reboots. Default: 6.1.',
            'ln_cpu': 'CPU frequency',
            'ln_cpu_hint': 'Lower clock, less supply noise. Check DSD512 and 768 kHz for dropouts. Default: 1608 MHz on 5.10, 1104 MHz on 6.1.',
            'ln_cpu_now': 'Now %s MHz.',
            'ln_default': 'Default',
            'ln_level': 'Level %s',
            'ln_drive': 'I2S pin drive',
            'ln_drive_hint': 'Edge speed of SCLK, LRCK and the data outputs. Lower: less ringing and ground bounce; higher for long wires. Default: level 2.',
            'ln_mclk_drive': 'MCLK pin drive',
            'ln_mclk_drive_hint': 'Edge speed of the MCLK output. No effect in EXT mode, where MCLK is an input. Default: level 3.',
            'ln_dma': 'DMA periods per buffer',
            'ln_dma_hint': 'Fewer, longer periods: fewer interrupts. Applies when playback starts. Default: 16.',
            'ln_dma_reboot': 'Reboot to enable.',
            'ln_scream': 'Scream resampling',
            'ln_scream_hint': 'Off: a sample rate the output does not take fails instead of being resampled. Default: On.',
            'ln_off': 'Off',
            'ln_on': 'On',
            'ln_error': 'Could not apply the setting'
        },
        'de': {
            'alsa_output': 'ALSA Ausgang:',
            'i2s_settings': 'I2S Einstellungen',
            'update_firmware': 'Firmware aktualisieren',
            'update_firmware_short': 'Update',
            'firmware_update': 'Firmware-Update',
            'close': 'Schließen',
            'update_start': 'Update wird gestartet...',
            'update_finish': 'Update abgeschlossen. Bitte LuckFox neu starten',
            'update_error': 'Update-Fehler.',
            'confirm_update': 'Sind Sie sicher, dass Sie die Firmware aktualisieren möchten?',
            'alsa_error': 'Fehler beim Umschalten von ALSA',
            'service_error': 'Fehler beim Umschalten des Dienstes',
            'service_switch_in_progress': 'Der Playerwechsel wird noch ausgeführt. Bitte warten.',
            'settings': '',
            'switching_player': 'Player wird gewechselt...',
            'switching_output': 'Ausgang wird gewechselt...',
            'switching_input': 'Eingang wird gewechselt...',
            'usb_dac_missing': 'USB-DAC nicht erkannt.<br>Bitte USB-DAC anschließen',
            'confirm_reboot': 'Sind Sie sicher, dass Sie das System neu starten möchten?',
            'confirm_shutdown': 'Sind Sie sicher, dass Sie das System herunterfahren möchten?',
            'shutdown_complete': 'System wurde heruntergefahren. Sie können die Stromversorgung trennen.',
            'i2s_title': 'I2S Einstellungen',
            'back_button': '← Zurück',
            'mode_title': 'Modus',
            'pll_mode': 'PLL',
            'ext_mode': 'EXT',
            'submode_title': 'Ausgangsmodus',
            'lr_mode': 'L/R',
            'plr_mode': '±L/±R',
            'std_mode': 'STD',
            'l_mode': 'L',
            'r_mode': 'R',
            'plusl_mode': '±L',
            'plusr_mode': '±R',
            '8ch_mode': '8CH',
            'mclk_title': 'MCLK',
            'warning_attention': 'Achtung!',
            'warning_text1': 'MCLK-Ausgang hat unterschiedliche Einstellungen in PLL- und EXT-Modi (OUTPUT/INPUT).',
            'warning_text2': 'Systemneustart ist erforderlich, nachdem I2S-Einstellungen geändert wurden.',
            'yes_btn': 'Ja',
            'cancel_btn': 'Abbrechen',
            'apply_reboot': 'Anwenden & Neustart',
            'pcm_swap_title': 'PCM Swap',
            'dsd_swap_title': 'DSD Swap',
            'freq_swap_title': '44/48 Swap',
            'leftjust_title': 'LeftJust',
            'confirm_kernel': 'LuckFox mit Linux-Kernel %s neu starten?<br>Der Kernel wird in den Flash geschrieben, Strom nicht trennen.',
            'switching_kernel': 'Kernel wird gewechselt, Neustart...',
            'kernel_error': 'Kernelwechsel fehlgeschlagen',
            'kernel_usb_to_i2s': 'USB to I2S funktioniert nur mit Kernel 6.1. Schalten Sie USB to I2S zuerst aus.',
            'usb_to_i2s_kernel': 'USB to I2S funktioniert nur mit Linux-Kernel 6.1',
            'kernel_caption': 'Kernel',
            'kernel_pf16': '5.10 mit dem I2S-Treiber von PureFox 1.6',
            'tab_player': 'Player',
            'tab_tweaks': 'Tweaks',
            'ln_experimental': 'Experimentell: Nutzung auf eigene Gefahr.',
            'kernel_info_6.1': 'Kernel 6.1. Funktioniert: DSD bis DSD1024 über I2S, USB to I2S. Die CPU läuft standardmäßig mit 1104 MHz.',
            'kernel_info_5.10': 'Kernel 5.10. Funktioniert: DSD bis DSD1024 über I2S, Ausgabe an einen USB-DAC. Funktioniert nicht: USB to I2S (der USB-Port ist nur Host). Die CPU läuft standardmäßig mit 1608 MHz.',
            'kernel_info_1.6': 'Kernel 5.10 mit dem alten I2S-Treiber von PureFox 1.6. Funktioniert: DSD bis DSD512 über I2S, Ausgabe an einen USB-DAC. Funktioniert nicht: DSD1024, USB to I2S (der USB-Port ist nur Host). Die CPU läuft standardmäßig mit 1608 MHz.',
            'ln_intro': 'Diese Einstellungen ändern die Audiodaten nicht. Vergleichen Sie nach Gehör, eine nach der anderen.',
            'ln_kernel_hint': 'Schreibt den Kernel in den Flash und startet neu. Standard: 6.1.',
            'ln_cpu': 'CPU-Takt',
            'ln_cpu_hint': 'Niedrigerer Takt, weniger Störungen auf der Versorgung. DSD512 und 768 kHz auf Aussetzer prüfen. Standard: 1608 MHz bei 5.10, 1104 MHz bei 6.1.',
            'ln_cpu_now': 'Jetzt %s MHz.',
            'ln_default': 'Standard',
            'ln_level': 'Stufe %s',
            'ln_drive': 'I2S-Treiberstärke',
            'ln_drive_hint': 'Flankensteilheit von SCLK, LRCK und den Datenausgängen. Niedriger: weniger Überschwingen und Masseprellen; höher für lange Leitungen. Standard: Stufe 2.',
            'ln_mclk_drive': 'MCLK-Treiberstärke',
            'ln_mclk_drive_hint': 'Flankensteilheit des MCLK-Ausgangs. Im EXT-Modus ohne Wirkung, dort ist MCLK ein Eingang. Standard: Stufe 3.',
            'ln_dma': 'DMA-Perioden pro Puffer',
            'ln_dma_hint': 'Weniger, längere Perioden: weniger Interrupts. Gilt ab dem nächsten Wiedergabestart. Standard: 16.',
            'ln_dma_reboot': 'Zum Aktivieren neu starten.',
            'ln_scream': 'Scream-Resampling',
            'ln_scream_hint': 'Aus: Eine vom Ausgang nicht unterstützte Abtastrate führt zu einem Fehler statt zu Resampling. Standard: Ein.',
            'ln_off': 'Aus',
            'ln_on': 'Ein',
            'ln_error': 'Einstellung konnte nicht übernommen werden'
        },
        'fr': {
            'alsa_output': 'Sortie ALSA:',
            'i2s_settings': 'Paramètres I2S',
            'update_firmware': 'Mettre à jour le firmware',
            'update_firmware_short': 'Mettre à jour',
            'firmware_update': 'Mise à jour du firmware',
            'close': 'Fermer',
            'update_start': 'Démarrage de la mise à jour...',
            'update_finish': 'Mise à jour terminée. Veuillez redémarrer LuckFox',
            'update_error': 'Erreur de mise à jour.',
            'confirm_update': 'Êtes-vous sûr de vouloir mettre à jour le firmware?',
            'alsa_error': 'Erreur lors du changement ALSA',
            'service_error': 'Erreur lors du changement de service',
            'service_switch_in_progress': 'Le changement de lecteur est toujours en cours. Veuillez patienter.',
            'settings': '',
            'switching_player': 'Changement de lecteur...',
            'switching_output': 'Changement de sortie...',
            'switching_input': 'Changement d\'entrée...',
            'usb_dac_missing': 'DAC USB non détecté.<br>Veuillez connecter un DAC USB',
            'confirm_reboot': 'Êtes-vous sûr de vouloir redémarrer le système?',
            'confirm_shutdown': 'Êtes-vous sûr de vouloir arrêter le système?',
            'shutdown_complete': 'Système arrêté. Vous pouvez débrancher l\'alimentation.',
            'i2s_title': 'Paramètres I2S',
            'back_button': '← Retour',
            'mode_title': 'Mode',
            'pll_mode': 'PLL',
            'ext_mode': 'EXT',
            'submode_title': 'Mode de sortie',
            'lr_mode': 'L/R',
            'plr_mode': '±L/±R',
            'std_mode': 'STD',
            'l_mode': 'L',
            'r_mode': 'R',
            'plusl_mode': '±L',
            'plusr_mode': '±R',
            '8ch_mode': '8CH',
            'mclk_title': 'MCLK',
            'warning_attention': 'Attention!',
            'warning_text1': 'La sortie MCLK a des paramètres différents en modes PLL et EXT (OUTPUT/INPUT).',
            'warning_text2': 'Un redémarrage du système est nécessaire après modification des paramètres I2S.',
            'yes_btn': 'Oui',
            'cancel_btn': 'Annuler',
            'apply_reboot': 'Appliquer et redémarrer',
            'pcm_swap_title': 'PCM Swap',
            'dsd_swap_title': 'DSD Swap',
            'freq_swap_title': '44/48 Swap',
            'leftjust_title': 'LeftJust',
            'confirm_kernel': 'Redémarrer LuckFox avec le noyau Linux %s ?<br>Le noyau est écrit en mémoire flash, ne coupez pas l\'alimentation.',
            'switching_kernel': 'Changement de noyau et redémarrage...',
            'kernel_error': 'Échec du changement de noyau',
            'kernel_usb_to_i2s': 'USB to I2S ne fonctionne qu\'avec le noyau 6.1. Désactivez d\'abord USB to I2S.',
            'usb_to_i2s_kernel': 'USB to I2S ne fonctionne qu\'avec le noyau Linux 6.1',
            'kernel_caption': 'Noyau',
            'kernel_pf16': '5.10 avec le pilote I2S de PureFox 1.6',
            'tab_player': 'Lecteur',
            'tab_tweaks': 'Réglages',
            'ln_experimental': 'Expérimental : à utiliser à vos propres risques.',
            'kernel_info_6.1': 'Noyau 6.1. Fonctionne : DSD jusqu\'à DSD1024 en I2S, USB to I2S. Le processeur tourne par défaut à 1104 MHz.',
            'kernel_info_5.10': 'Noyau 5.10. Fonctionne : DSD jusqu\'à DSD1024 en I2S, sortie vers un DAC USB. Ne fonctionne pas : USB to I2S (le port USB est uniquement hôte). Le processeur tourne par défaut à 1608 MHz.',
            'kernel_info_1.6': 'Noyau 5.10 avec l\'ancien pilote I2S de PureFox 1.6. Fonctionne : DSD jusqu\'à DSD512 en I2S, sortie vers un DAC USB. Ne fonctionne pas : DSD1024, USB to I2S (le port USB est uniquement hôte). Le processeur tourne par défaut à 1608 MHz.',
            'ln_intro': 'Ces réglages ne modifient pas les données audio. Comparez-les à l\'oreille, un par un.',
            'ln_kernel_hint': 'Écrit le noyau en mémoire flash et redémarre. Par défaut : 6.1.',
            'ln_cpu': 'Fréquence du processeur',
            'ln_cpu_hint': 'Horloge plus basse, moins de bruit sur l\'alimentation. Vérifiez DSD512 et 768 kHz (coupures). Par défaut : 1608 MHz avec 5.10, 1104 MHz avec 6.1.',
            'ln_cpu_now': 'Actuellement %s MHz.',
            'ln_default': 'Par défaut',
            'ln_level': 'Niveau %s',
            'ln_drive': 'Courant des sorties I2S',
            'ln_drive_hint': 'Raideur des fronts de SCLK, LRCK et des sorties de données. Plus bas : moins de suroscillations et de rebond de masse ; plus haut pour des fils longs. Par défaut : niveau 2.',
            'ln_mclk_drive': 'Courant de la sortie MCLK',
            'ln_mclk_drive_hint': 'Raideur des fronts de la sortie MCLK. Sans effet en mode EXT, où MCLK est une entrée. Par défaut : niveau 3.',
            'ln_dma': 'Périodes DMA par tampon',
            'ln_dma_hint': 'Moins de périodes, plus longues : moins d\'interruptions. S\'applique au prochain démarrage de la lecture. Par défaut : 16.',
            'ln_dma_reboot': 'Redémarrez pour l\'activer.',
            'ln_scream': 'Rééchantillonnage Scream',
            'ln_scream_hint': 'Off : une fréquence non prise en charge par la sortie provoque une erreur au lieu d\'être rééchantillonnée. Par défaut : On.',
            'ln_off': 'Off',
            'ln_on': 'On',
            'ln_error': 'Impossible d\'appliquer le réglage'
        },
        'zh': {
            'alsa_output': 'ALSA 输出:',
            'i2s_settings': 'I2S 设置',
            'update_firmware': '更新固件',
            'update_firmware_short': '更新',
            'firmware_update': '固件更新',
            'close': '关闭',
            'update_start': '开始更新...',
            'update_finish': '更新完成。请重启 LuckFox',
            'update_error': '更新错误。',
            'confirm_update': '您确定要更新固件吗？',
            'alsa_error': 'ALSA 切换错误',
            'service_error': '服务切换错误',
            'service_switch_in_progress': '播放器仍在切换中，请稍候。',
            'settings': '',
            'switching_player': '正在切换播放器...',
            'switching_output': '正在切换输出...',
            'switching_input': '正在切换输入...',
            'usb_dac_missing': '未检测到 USB DAC。<br>请连接 USB DAC',
            'confirm_reboot': '您确定要重启系统吗？',
            'confirm_shutdown': '您确定要关闭系统吗？',
            'shutdown_complete': '系统已关闭。您可以断开电源。',
            'i2s_title': 'I2S 设置',
            'back_button': '← 返回',
            'mode_title': '主模式',
            'pll_mode': 'PLL',
            'ext_mode': 'EXT',
            'submode_title': '输出模式',
            'lr_mode': 'L/R',
            'plr_mode': '±L/±R',
            'std_mode': 'STD',
            'l_mode': 'L',
            'r_mode': 'R',
            'plusl_mode': '±L',
            'plusr_mode': '±R',
            '8ch_mode': '8CH',
            'mclk_title': 'MCLK',
            'warning_attention': '注意！',
            'warning_text1': 'MCLK输出在PLL和EXT模式下具有不同的设置（OUTPUT/INPUT）。',
            'warning_text2': '更改I2S设置后需要重启系统才能生效。',
            'yes_btn': '是',
            'cancel_btn': '取消',
            'apply_reboot': '应用并重启',
            'pcm_swap_title': 'PCM Swap',
            'dsd_swap_title': 'DSD Swap',
            'freq_swap_title': '44/48 Swap',
            'leftjust_title': 'LeftJust',
            'confirm_kernel': '使用 Linux 内核 %s 重启 LuckFox？<br>内核将写入闪存，请勿断电。',
            'switching_kernel': '正在切换内核并重启...',
            'kernel_error': '切换内核失败',
            'kernel_usb_to_i2s': 'USB to I2S 仅支持 6.1 内核。请先关闭 USB to I2S。',
            'usb_to_i2s_kernel': 'USB to I2S 仅支持 Linux 6.1 内核',
            'kernel_caption': '内核',
            'kernel_pf16': '5.10，使用 PureFox 1.6 的 I2S 驱动',
            'tab_player': '播放器',
            'tab_tweaks': '调校',
            'ln_experimental': '实验性功能：使用风险自负。',
            'kernel_info_6.1': '内核 6.1。可用：I2S 输出最高 DSD1024，USB to I2S。CPU 默认运行在 1104 MHz。',
            'kernel_info_5.10': '内核 5.10。可用：I2S 输出最高 DSD1024，输出到 USB DAC。不可用：USB to I2S（USB 口仅作主机）。CPU 默认运行在 1608 MHz。',
            'kernel_info_1.6': '内核 5.10，使用 PureFox 1.6 的旧 I2S 驱动。可用：I2S 输出最高 DSD512，输出到 USB DAC。不可用：DSD1024，USB to I2S（USB 口仅作主机）。CPU 默认运行在 1608 MHz。',
            'ln_intro': '这些设置不会改变音频数据。请逐项用耳朵比较。',
            'ln_kernel_hint': '将内核写入闪存并重启。默认：6.1。',
            'ln_cpu': 'CPU 频率',
            'ln_cpu_hint': '频率越低，电源噪声越小。请检查 DSD512 和 768 kHz 是否断音。默认：5.10 为 1608 MHz，6.1 为 1104 MHz。',
            'ln_cpu_now': '当前 %s MHz。',
            'ln_default': '默认',
            'ln_level': '等级 %s',
            'ln_drive': 'I2S 引脚驱动强度',
            'ln_drive_hint': 'SCLK、LRCK 和数据输出的边沿速度。较低：振铃和地弹更少；长导线时调高。默认：等级 2。',
            'ln_mclk_drive': 'MCLK 引脚驱动强度',
            'ln_mclk_drive_hint': 'MCLK 输出的边沿速度。EXT 模式下无效（MCLK 为输入）。默认：等级 3。',
            'ln_dma': '每个缓冲区的 DMA 周期数',
            'ln_dma_hint': '周期更少、更长：中断更少。从下次开始播放时生效。默认：16。',
            'ln_dma_reboot': '重启后启用。',
            'ln_scream': 'Scream 重采样',
            'ln_scream_hint': '关：输出不支持的采样率将报错，而不是被重采样。默认：开。',
            'ln_off': '关',
            'ln_on': '开',
            'ln_error': '无法应用设置'
        }
    };

    // Browser language detection (PRESERVED!)
    window.detectLanguage = function() {
        const lang = navigator.language || navigator.userLanguage;
        if (lang.startsWith('ru')) return 'ru';
        if (lang.startsWith('de')) return 'de';
        if (lang.startsWith('fr')) return 'fr';
        if (lang.startsWith('zh')) return 'zh';
        return 'en';
    }

    // Language application (PRESERVED!)
    window.currentLang = window.detectLanguage();

    window.applyTranslations = function() {
        $('[data-lang]').each(function() {
            const key = $(this).data('lang');
            if (window.translations[window.currentLang][key] !== undefined) {
                if (key !== 'settings' || !$(this).find('img').length) {
                    $(this).text(window.translations[window.currentLang][key]);
                }
            }
        });
    }

    window.applyTranslations();

    // Adaptive styles (FULLY PRESERVED!)
    function setResponsiveStyles() {
        if (window.innerWidth < 500) {
            $('.player-buttons, .ap-buttons, .streaming-buttons').css({
                'display': 'block',
                'gap': '0'
            });
            $('.player-buttons .btn-custom, .ap-buttons .btn-custom, .streaming-buttons .btn-custom').css({
                'width': '100%',
                'margin-top': '10px',
                'margin-bottom': '0'
            });
            $('.alsa-button').css('padding', '10px');
        } else {
            $('.player-buttons, .ap-buttons, .streaming-buttons').css({
                'display': 'flex',
                'gap': '10px'
            });
            $('.player-buttons .btn-custom, .ap-buttons .btn-custom, .streaming-buttons .btn-custom').css({
                'width': 'calc(50% - 5px)',
                'margin-top': '0',
                'margin-bottom': '0'
            });
            $('.alsa-button').css('padding', '10px 20px');
        }
    }

    setResponsiveStyles();
    $(window).resize(function() {
        setResponsiveStyles();
    });
    
    // Check CSS gap support
    function checkGapSupport() {
        const testElement = document.createElement('div');
        testElement.style.display = 'flex';
        testElement.style.gap = '10px';
        if (testElement.style.gap === '10px') {
            document.documentElement.classList.add('supports-gap');
        }
    }
    checkGapSupport();

    // Force status check on user actions
    function forceStatusCheck() {
        const requestSequence = ++statusRequestSequence;

        debugLog('Принудительная проверка состояния...');
        $.ajax({
            url: 'status_fast.php?_=' + Date.now(),
            method: 'GET',
            timeout: 3000,
            dataType: 'json',
            cache: false,
            success: function(response) {
                if (requestSequence !== statusRequestSequence) {
                    return;
                }
                debugLog('Принудительная проверка:', response);
                handleBootIdentity(response);
                updateInterfaceFromStatus(response);
                lastKnownStatus = response;
            },
            error: function() {
                debugWarn('Ошибка принудительной проверки');
            }
        });
    }

    // Refresh status from backend after delayed service transitions.
    function updateStatusFromServer() {
        $.ajax({
            url: 'status_fast.php',
            method: 'GET',
            timeout: 3000,
            dataType: 'json',
            cache: false,
            success: function(response) {
                handleBootIdentity(response);
                lastKnownStatus = response;
                updateInterfaceFromStatus(response);
            },
            error: function(xhr, status, error) {
                console.error('Status refresh failed:', status, error);
            }
        });
    }

    // This is a short-lived fallback for an SSE reconnect during a switch.
    // status_fast.php only reads status_monitor's already prepared JSON.
    function probeServiceStart(service) {
        if (!isServiceSwitching || pendingService !== service) {
            return;
        }

        $.ajax({
            url: 'status_fast.php',
            method: 'GET',
            timeout: 1000,
            dataType: 'json',
            cache: false,
            success: function(response) {
                if (!isServiceSwitching || pendingService !== service) {
                    return;
                }
                lastKnownStatus = response;
                updateInterfaceFromStatus(response);
                if (isServiceSwitching) {
                    serviceStatusProbeTimer = setTimeout(function() {
                        probeServiceStart(service);
                    }, 250);
                }
            },
            error: function() {
                if (isServiceSwitching && pendingService === service) {
                    serviceStatusProbeTimer = setTimeout(function() {
                        probeServiceStart(service);
                    }, 250);
                }
            }
        });
    }

    function startStatusEvents() {
        if (!window.EventSource || statusEvents) {
            return;
        }

        statusEvents = new EventSource('status_events.php');
        statusEvents.onopen = function() {
            statusEventsConnected = true;
            if (statusRecoveryTimer) {
                clearTimeout(statusRecoveryTimer);
                statusRecoveryTimer = null;
            }
            // USBtoI2S enable/disable restarts status_monitor, so every
            // (re)connect is a good moment to re-read the mode state: it may
            // have been toggled outside this page (console/STM32, another tab).
            checkUsbToI2sStatus();
        };
        statusEvents.addEventListener('status', function(event) {
            try {
                const response = JSON.parse(event.data);
                statusEventsConnected = true;
                handleBootIdentity(response);
                lastKnownStatus = response;
                updateInterfaceFromStatus(response);
            } catch (error) {
                debugWarn('Invalid status event:', error);
            }
        });
        statusEvents.onerror = function() {
            statusEventsConnected = false;
            scheduleStatusRecovery();
        };
    }

    function handleBootIdentity(data) {
        if (!data || !data.boot_id) {
            return;
        }
        if (currentBootId && currentBootId !== data.boot_id) {
            beginRebootRecovery();
            return;
        }
        currentBootId = data.boot_id;
    }

    function beginRebootRecovery() {
        if (rebootRecoveryActive) {
            return;
        }
        rebootRecoveryActive = true;
        const rebootText = {
            'ru': 'Система перезагружается...',
            'de': 'System wird neu gestartet...',
            'fr': 'Le systeme redemarre...',
            'zh': '系统正在重启...',
            'en': 'System is rebooting...'
        };
        const language = detectLanguage();
        showSpinner(rebootText[language] || rebootText.en);
        setTimeout(function() {
            window.location.reload();
        }, 300);
    }

    function scheduleStatusRecovery() {
        if (statusRecoveryTimer || rebootRecoveryActive) {
            return;
        }
        statusRecoveryTimer = setTimeout(function() {
            statusRecoveryTimer = null;
            $.ajax({
                url: 'status_fast.php?_=' + Date.now(),
                method: 'GET',
                timeout: 1000,
                dataType: 'json',
                cache: false,
                success: function(response) {
                    handleBootIdentity(response);
                    lastKnownStatus = response;
                    updateInterfaceFromStatus(response);
                    restartStatusEvents();
                },
                error: scheduleStatusRecovery
            });
        }, 2000);
    }

    function restartStatusEvents() {
        if (statusEvents) {
            statusEvents.close();
            statusEvents = null;
        }
        statusEventsConnected = false;
        startStatusEvents();
    }

    // Track page visibility
    document.addEventListener('visibilitychange', function() {
        if (!document.hidden) {
            restartStatusEvents();
            forceStatusCheck();
        }
    });

    window.addEventListener('pageshow', function() {
        restartStatusEvents();
        forceStatusCheck();
    });

    // Spinner control (PRESERVED!)
    function showSpinner(text = null) {
        if (text) {
            $('.spinner-text').text(text);
        }
        $('.spinner-overlay').addClass('show');
    }

    function hideSpinner() {
        $('.spinner-overlay').removeClass('show');
    }

    // Check USB DAC via forced status check
    function checkUsbDac(successCallback, errorCallback) {
        // Always do fresh check for USB DAC - it can be connected/disconnected
        $.ajax({
            url: 'status_fast.php',
            method: 'GET',
            timeout: 3000,
            dataType: 'json',
            cache: false, // Принудительно отключаем кеширование
            success: function(response) {
                lastKnownStatus = response;
                updateInterfaceFromStatus(response);
                
                if (response.usb_dac) {
                    if (successCallback) successCallback();
                } else {
                    customAlert(translations[currentLang]['usb_dac_missing']);
                    if (errorCallback) errorCallback();
                }
            },
            error: function(xhr, status, error) {
                console.error('AJAX error in checkUsbDac:', status, error, 'Response:', xhr.responseText);
                customAlert(translations[currentLang]['service_error'] + ': ' + status);
                if (errorCallback) errorCallback();
            }
        });
    }

    // Принудительная проверка состояния сервисов (только по требованию)
    function checkActiveService(callback) {
        debugLog('Проверка активного сервиса...');
        $.ajax({
            url: 'status_fast.php',
            method: 'GET',
            timeout: 3000,
            dataType: 'json',
            success: function(response) {
                const activeService = response.active_service || '';
                
                lastKnownStatus = response;
                
                // Обновляем UI
                if (!isServiceSwitching && activeService !== previousActiveService) {
                    if (previousActiveService !== null && previousActiveService !== activeService) {
                        hideSpinner();
                    }
                    previousActiveService = activeService;
                    $('button[data-service]').removeClass('active');
                    if (activeService) {
                        $(`button[data-service="${activeService}"]`).addClass('active');
                    }
                }
                
                if (!$('#usbto-i2s-btn').hasClass('active')) {
                    updateAlsaUI(response.alsa_state);
                }
                
                if (callback) callback(activeService);
            },
            error: function() {
                debugWarn('Ошибка проверки состояния');
            }
        });
    }

    // Output selector UI update (usb / i2s / bridge)
    function updateAlsaUI(alsaState) {
        debugLog("updateAlsaUI called, state:", alsaState);
        const toggleInput = $('#alsa-toggle');
        const i2sSettingsLink = $('#i2s-settings-link');

        $('.alsa-toggle').removeClass('active-i2s');

        switch (alsaState) {
            case 'usb':
                toggleInput.prop('checked', false);
                i2sSettingsLink.addClass('no-transition').css({
                    'opacity': '0.3',
                    'pointer-events': 'none',
                    'cursor': 'not-allowed'
                });
                setTimeout(() => i2sSettingsLink.removeClass('no-transition'), 10);
                break;
            case 'i2s':
            case 'bridge':
                toggleInput.prop('checked', true);
                $('.alsa-toggle').addClass('active-i2s');
                i2sSettingsLink.addClass('no-transition').css({
                    'opacity': '1',
                    'pointer-events': 'auto',
                    'cursor': 'pointer'
                });
                setTimeout(() => i2sSettingsLink.removeClass('no-transition'), 10);
                break;
            case 'error':
                console.error('Ошибка при чтении конфигурации ALSA');
                break;
        }
    }

    // Проверка состояния ALSA через принудительную проверку
    function checkAlsaState() {
        // Используем последнее известное состояние
        if (lastKnownStatus && lastKnownStatus.alsa_state) {
            updateAlsaUI(lastKnownStatus.alsa_state);
            return;
        }
        
        // Иначе принудительно проверяем
        checkActiveService();
    }

    // ALSA toggle: USB ↔ I2S
    $('#alsa-toggle').change(function(e) {
        e.preventDefault();
        const checkbox = $(this);
        const isChecked = checkbox.is(':checked');
        const cardType = isChecked ? 'i2s' : 'usb';

        updateAlsaUI(cardType);
        isAlsaSwitching = true;
        forceStatusCheck();

        if (cardType === 'usb') {
            const doSwitch = function() {
                if (isDlnaBridgeActive) {
                    $.ajax({ url: 'handle_dlna.php', method: 'POST', data: { action: 'disable' },
                        success: function() { isDlnaBridgeActive = false; localStorage.removeItem('dlna_bridge_active'); $('#dlna-bridge-btn').removeClass('active'); switchAlsa('usb'); },
                        error: function() { isAlsaSwitching = false; forceStatusCheck(); }
                    });
                } else {
                    switchAlsa(cardType);
                }
            };
            checkUsbDac(doSwitch, function() {
                checkbox.prop('checked', !isChecked);
                updateAlsaUI(!isChecked ? 'i2s' : 'usb');
                isAlsaSwitching = false;
            });
        } else {
            switchAlsa(cardType);
        }
    });

    // DLNA Bridge button — toggle on/off
    $('#dlna-bridge-btn').click(function() {
        if (isDlnaBridgeActive) {
            isDlnaBridgeActive = false;
            localStorage.removeItem('dlna_bridge_active');
            $(this).removeClass('active');
            $.ajax({
                url: 'handle_dlna.php', method: 'POST', data: { action: 'disable' },
                success: function() { forceStatusCheck(); },
                error: function() {
                    isDlnaBridgeActive = true;
                    localStorage.setItem('dlna_bridge_active', 'true');
                    $('#dlna-bridge-btn').addClass('active');
                }
            });
        } else {
            isDlnaBridgeActive = true;
            localStorage.setItem('dlna_bridge_active', 'true');
            $(this).addClass('active');
            $.ajax({
                url: 'handle_dlna.php', method: 'POST', data: { action: 'enable' },
                timeout: 15000,
                success: function() { forceStatusCheck(); },
                error: function() {
                    isDlnaBridgeActive = false;
                    localStorage.removeItem('dlna_bridge_active');
                    $('#dlna-bridge-btn').removeClass('active');
                    customAlert('DLNA bridge enable failed');
                }
            });
        }
    });

    // Упрощенная функция переключения ALSA
    function switchAlsa(cardType) {
        // UI уже обновлен пользователем (toggle switch), просто отправляем команду
        $.ajax({
            url: 'handle_alsa.php',
            method: 'POST',
            data: { card: cardType },
            timeout: 10000,
            success: function() {
                // UI уже обновлен при клике, просто разблокируем обновления
                setTimeout(() => {
                    isAlsaSwitching = false;
                }, 2000);
            },
            error: function() {
                // При ошибке возвращаем toggle в исходное состояние
                const oppositeState = (cardType === 'usb') ? 'i2s' : 'usb';
                updateAlsaUI(oppositeState);
                isAlsaSwitching = false; // Разблокируем обновления
            }
        });
    }

    // Обработка кликов по кнопкам сервисов (ПОЛНОСТЬЮ СОХРАНЕНА!)
    $('.btn-custom').click(function(e) {
        if ($(e.target).is('a') || $(e.target).is('img')) return true;
        if (!$(this).data('service')) return;
        
        // Ignore clicks that follow a swipe gesture — the user was
        // hiding the button, not requesting a player switch.
        if (window._purefoxSwipeInProgress) {
            window._purefoxSwipeInProgress = false;
            return;
        }
        
        if (isServiceSwitching || window.purefoxServiceSwitchInProgress) {
            customAlert(translations[currentLang]['service_switch_in_progress']);
            return;
        }

        if ($(this).hasClass('active')) return;

        const service = $(this).data('service');
        
        // Используем последнее известное состояние ALSA
        const alsaState = lastKnownStatus ? lastKnownStatus.alsa_state : null;
        if (alsaState === 'usb') {
            checkUsbDac(function() { switchPlayerService(service); });
        } else {
            switchPlayerService(service);
        }
    });

    // Функция переключения сервиса с мгновенной активацией кнопки
    function switchPlayerService(service) {
        // Блокируем обновления кнопок во время переключения
        isServiceSwitching = true;
        window.purefoxServiceSwitchInProgress = true;
        pendingService = service;
        if (serviceSwitchWatchdog) clearTimeout(serviceSwitchWatchdog);
        serviceSwitchWatchdog = setTimeout(function() {
            resetServiceSwitchFlag();
        }, 45000);

        // Unlock ALSA toggle and deactivate USBtoI2S button when switching to any player
        unlockAlsaToggle();
        $('#usbto-i2s-btn').removeClass('active');

        // СРАЗУ делаем кнопку активной для отзывчивости UI
        $('button[data-service]').removeClass('active');
        $(`button[data-service="${service}"]`).addClass('active');
        debugLog('Кнопка', service, 'активирована мгновенно, ожидаем запуск сервиса...');
        probeServiceStart(service);

        // Переключение сервиса
        
        // Увеличенный таймаут для сервисов с двухэтапным запуском
        const timeoutDuration = 8000;
        

        $.ajax({
            url: 'handle_service.php',
            method: 'POST',
            data: { service: service },
            timeout: 15000,
            dataType: 'json',
            success: function(result) {
                if (!result || result.status !== 'success') {
                    console.error('Service switch rejected:', result);
                    resetServiceSwitchFlag();
                    $('.btn-custom').removeClass('active');
                    customAlert((result && result.message) || translations[currentLang]['service_error']);
                    return;
                }
                if (!isServiceSwitching || pendingService !== service) {
                    return;
                }
                debugLog('Команда переключения на', service, 'отправлена, начинаем проверку...');
                
                // Сразу начинаем проверку без задержки
                const checkInterval = 500; // 500ms между проверками
                let checkCount = 0;
                const maxChecks = 60; // 20 секунд максимум

                function checkServiceStatusChange() {
                        $.ajax({
                            url: 'status_fast.php', // Используем оптимизированный запрос
                            method: 'GET',
                            timeout: 3000,
                            dataType: 'json',
                            success: function(response) {
                                const activeService = response.active_service || '';
                                checkCount++;
                                
                                debugLog('Проверка', checkCount, ': активный сервис =', activeService, ', ожидаемый =', service);
                                
                                if (!activeService) {
                                    debugWarn('Нет активных сервисов, продолжаем проверку... (попытка', checkCount, 'из', maxChecks, ')');
                                    if (checkCount >= maxChecks) {
                                        console.error('Сервис', service, 'не поднялся после максимального числа попыток');
                                        resetServiceSwitchFlag();
                                        $('.btn-custom').removeClass('active');
                                        customAlert(translations[currentLang]['service_error']);
                                        return;
                                    }
                                    // Продолжаем проверку, кнопка остается активной
                                    setTimeout(checkServiceStatusChange, checkInterval);
                                    return;
                                }
                                
                                if (activeService === service) {
                                    debugLog('Успешно переключен на', service);
                                    resetServiceSwitchFlag();
                                    lastKnownStatus = response;
                                    updateInterfaceFromStatus(response);
                                    debugLog('Сервис переключен успешно');

                                    // Принудительно обновляем данные из system_status.json через 2.5s
                                    // (учитываем задержку обновления status_monitor ~2s)
                                    setTimeout(() => {
                                        debugLog('Обновление данных после переключения источника...');
                                        updateStatusFromServer();
                                    }, 2500);
                                } else if (checkCount >= maxChecks) {
                                    console.error("Тайм-аут при переключении на сервис " + service);
                                    resetServiceSwitchFlag();
                                    $('.btn-custom').removeClass('active');
                                    customAlert(translations[currentLang]['service_error']);
                                } else {
                                    // The old player can remain in status_monitor briefly
                                    // after its process has been stopped. Keep waiting for
                                    // the requested service instead of reverting the UI.
                                    debugLog("Ожидаем " + service + ", пока активен " + activeService);
                                    setTimeout(checkServiceStatusChange, checkInterval);
                                }
                            },
                            error: function(xhr, status, error) {
                                checkCount++;
                                debugWarn('Ошибка проверки состояния:', status, error);
                                if (checkCount >= maxChecks) {
                                    console.error('Тайм-аут проверки состояния:', status, error);
                                    resetServiceSwitchFlag();
                                    $('.btn-custom').removeClass('active');
                                    customAlert(translations[currentLang]['service_error']);
                                    return;
                                }
                                setTimeout(checkServiceStatusChange, checkInterval);
                            }
                        });
                    }

                // Запускаем проверку сразу
                checkServiceStatusChange();
            },
            error: function(xhr, status, error) {
                console.error('AJAX error switching to', service, ':', status, error, 'Response:', xhr.responseText);
                resetServiceSwitchFlag();
                $('.btn-custom').removeClass('active');
                customAlert(translations[currentLang]['service_error'] + ': ' + status);
            },
            complete: function() {
                window.purefoxServiceSwitchInProgress = false;
            }
        });
    }

    // Инициализация
    checkActiveService(function(activeService) {
        previousActiveService = activeService;
    });
    checkAlsaState();

    // Кастомный confirm диалог
    function customConfirm(message, callback) {
        $('#confirm-message').html(message);
        $('#custom-confirm').addClass('show');
        
        // Обновляем текст кнопок на основе языка
        const yesText = {
            'ru': 'Да',
            'de': 'Ja',
            'fr': 'Oui',
            'zh': '是',
            'en': 'Yes'
        };
        const cancelText = {
            'ru': 'Отмена',
            'de': 'Abbrechen',
            'fr': 'Annuler',
            'zh': '取消',
            'en': 'Cancel'
        };
        $('#confirm-yes').text(yesText[currentLang] || yesText['en']);
        $('#confirm-no').text(cancelText[currentLang] || cancelText['en']);
        
        // Обработчики кнопок
        $('#confirm-yes').off('click').on('click', function() {
            $('#custom-confirm').removeClass('show');
            callback(true);
        });
        
        $('#confirm-no').off('click').on('click', function() {
            $('#custom-confirm').removeClass('show');
            callback(false);
        });
    }
    
    function customAlert(message) {
        $('#alert-message').html(message);
        $('#custom-alert').addClass('show');
        
        // Обновляем текст кнопки на основе языка
        $('#alert-ok').text('OK');
        
        // Обработчик кнопки
        $('#alert-ok').off('click').on('click', function() {
            $('#custom-alert').removeClass('show');
        });
        
        // Закрытие по клику на фон
        $('#custom-alert').off('click').on('click', function(e) {
            if (e.target === this) {
                $('#custom-alert').removeClass('show');
            }
        });
    }

    // ПОЛНОСТЬЮ СОХРАНЕНА функция обновления прошивки!
    $('#update-firmware').click(function(e) {
        
        customConfirm(translations[currentLang]['confirm_update'], function(confirmed) {
            if (!confirmed) return;

        $('#update-log-modal').addClass('show');
        $('#update-log-content').text(translations[currentLang]['update_start'] + "\n");
        let logContent = $('#update-log-content');

        $.ajax({
            url: 'run_update.php',
            method: 'GET',
            xhrFields: {
                onprogress: function(e) {
                    let newText = e.currentTarget.responseText;
                    logContent.append(newText);
                    setTimeout(() => { logContent.scrollTop(logContent.prop("scrollHeight")); }, 100);
                }
            },
            success: function() {
                logContent.html(logContent.html());
                logContent.append('\n<span class="update-finish">' + translations[currentLang]['update_finish'] + '</span>');
            },
            error: function() {
                logContent.append('\n<span class="update-error">' + translations[currentLang]['update_error'] + '</span>');
                logContent.html(logContent.html());
            }
        });
        });
    });

    // СОХРАНЕНА функция закрытия модального окна
    $('.close-modal').click(function() {
        $('#update-log-modal').removeClass('show');
    });

    // Обработчики для reboot и shutdown
    $('#reboot-link').click(function(e) {
        e.preventDefault();
        customConfirm(translations[currentLang]['confirm_reboot'], function(confirmed) {
            if (confirmed) {
            $('.spinner-overlay').addClass('show');
            const rebootText = {
                'ru': 'Перезагрузка...',
                'de': 'Neustart...',
                'fr': 'Redémarrage...',
                'zh': '重启中...',
                'en': 'Rebooting...'
            };
            $('.spinner-text').text(rebootText[currentLang] || rebootText['en']);
            
            $.ajax({
                url: 'reboot.php',
                method: 'POST',
                success: function() {
                    // Ждем восстановления соединения после перезагрузки
                    setTimeout(checkConnectionAfterReboot, 3000);
                },
                error: function() {
                    // Если запрос не прошел, все равно ждем восстановления
                    setTimeout(checkConnectionAfterReboot, 3000);
                }
            });
            }
        });
    });

    $('#shutdown-link').click(function(e) {
        e.preventDefault();
        customConfirm(translations[currentLang]['confirm_shutdown'], function(confirmed) {
            if (confirmed) {
            $('.spinner-overlay').addClass('show');
            const shutdownText = {
                'ru': 'Выключение...',
                'de': 'Herunterfahren...',
                'fr': 'Arrêt en cours...',
                'zh': '关机中...',
                'en': 'Shutting down...'
            };
            $('.spinner-text').text(shutdownText[currentLang] || shutdownText['en']);
            
            $.ajax({
                url: 'shutdown.php',
                method: 'POST',
                success: function() {
                    // Показываем сообщение о завершении через 3 секунды
                    setTimeout(function() {
                        $('.spinner').hide();
                        $('.spinner-text').text(translations[currentLang]['shutdown_complete']);
                    }, 3000);
                },
                error: function() {
                    const errorText = {
                        'ru': 'Ошибка при выключении',
                        'de': 'Fehler beim Herunterfahren',
                        'fr': 'Erreur d\'arrêt',
                        'zh': '关机错误',
                        'en': 'Shutdown error'
                    };
                    customAlert(errorText[currentLang] || errorText['en']);
                    $('.spinner-overlay').removeClass('show');
                }
            });
            }
        });
    });

    // Linux kernel 6.1 / 5.10 / 1.6 (kernel.php, /opt/kernel.sh)
    function kernelName(kernel) {
        return kernel === '1.6' ? translations[currentLang]['kernel_pf16'] : kernel;
    }

    function showKernel(kernel) {
        $('.kernel-option').removeClass('active')
            .filter('[data-kernel="' + kernel + '"]').addClass('active');
    }

    function loadKernelStatus() {
        $.ajax({
            url: 'kernel.php',
            method: 'GET',
            dataType: 'json',
            cache: false,
            success: function(st) {
                $('.kernel-option').each(function() {
                    const kernel = $(this).data('kernel').toString();
                    $(this).prop('hidden', st.available.indexOf(kernel) < 0 && kernel !== st.selected);
                });
                $('.kernel-option[data-kernel="1.6"]').attr('title', translations[currentLang]['kernel_pf16']);
                showKernel(st.selected);
                $('#ln-kernel-info').text(translations[currentLang]['kernel_info_' + st.selected] || '')
                    .prop('hidden', false);
                $('#kernel-row').prop('hidden', !(st.available.length > 1 || st.selected !== '6.1'));
                // the USBtoI2S gadget needs the 6.1 kernel
                $('#usbto-i2s-btn').prop('disabled', st.running !== '6.1')
                    .attr('title', st.running !== '6.1' ? translations[currentLang]['usb_to_i2s_kernel'] : null);
            }
        });
    }
    loadKernelStatus();

    $('.kernel-option').click(function() {
        const button = $(this);
        const target = button.data('kernel').toString();
        if (button.hasClass('active')) {
            return;
        }
        const previous = $('.kernel-option.active').data('kernel');

        if (target !== '6.1' && $('#usbto-i2s-btn').hasClass('active')) {
            customAlert(translations[currentLang]['kernel_usb_to_i2s']);
            return;
        }
        customConfirm(translations[currentLang]['confirm_kernel'].replace('%s', kernelName(target)), function(confirmed) {
            if (!confirmed) {
                return;
            }
            showKernel(target);
            showSpinner(translations[currentLang]['switching_kernel']);
            $.ajax({
                url: 'kernel.php',
                method: 'POST',
                data: { kernel: target },
                dataType: 'json',
                timeout: 120000,
                success: function() {
                    setTimeout(checkConnectionAfterReboot, 5000);
                },
                error: function(xhr) {
                    hideSpinner();
                    showKernel(String(previous));
                    const detail = (xhr.responseJSON && xhr.responseJSON.message) || '';
                    customAlert(translations[currentLang]['kernel_error'] +
                        (detail ? '<br>' + $('<div>').text(detail).html().replace(/\n/g, '<br>') : ''));
                }
            });
        });
    });

    // Tabs: Player | Tweaks; the open tab is remembered in this browser
    function showTab(tab) {
        $('.tab-btn').removeClass('active').filter('[data-tab="' + tab + '"]').addClass('active');
        $('.tab-panel').prop('hidden', true);
        $('#tab-' + tab).prop('hidden', false);
        if (tab === 'tweaks') {
            loadLowNoise();
        }
    }

    $('.tab-btn').click(function() {
        const tab = $(this).data('tab');
        try { localStorage.setItem('purefox_tab', tab); } catch (e) { /* storage blocked */ }
        showTab(tab);
    });

    // Low noise settings (lownoise.php, /opt/lownoise.sh)
    function lnText(key) {
        return translations[currentLang][key] || translations['en'][key];
    }

    function showLowNoise(st) {
        const s = st.settings;

        const cpu = $('#ln-cpu').empty();
        $('<option>').val('0').text(lnText('ln_default')).appendTo(cpu);
        st.cpu.frequencies.slice().sort(function(a, b) { return b - a; }).forEach(function(khz) {
            $('<option>').val(String(khz)).text((khz / 1000) + ' MHz').appendTo(cpu);
        });
        cpu.val(s.CPU_MAX_FREQ);
        $('#ln-cpu-row').prop('hidden', st.cpu.frequencies.length === 0);
        $('#ln-cpu-now').text(st.cpu.current ? lnText('ln_cpu_now').replace('%s', st.cpu.current / 1000) : '');

        $('.ln-drive').each(function() {
            const drive = $(this).empty();
            $('<option>').val('default').text(lnText('ln_default')).appendTo(drive);
            for (let level = 0; level <= 7; level++) {
                $('<option>').val(String(level)).text(lnText('ln_level').replace('%s', level)).appendTo(drive);
            }
            drive.val(s[drive.data('key')]);
        });
        // MCLK is an input in EXT mode, its drive has no effect
        $('#ln-mclk-drive').prop('disabled', st.i2s_mode === 'ext');

        $('.ln-option').each(function() {
            const key = $(this).data('key');
            $(this).toggleClass('active', String($(this).data('value')) === s[key]);
        });
        $('#ln-dma .ln-option').prop('disabled', !st.dma_supported);
        $('#ln-dma-reboot').prop('hidden', st.dma_supported);
        // Only Scream reads this setting
        $('#ln-scream .ln-option').prop('disabled', !st.scream_active);
    }

    function loadLowNoise() {
        $.ajax({
            url: 'lownoise.php',
            method: 'GET',
            dataType: 'json',
            cache: false,
            success: showLowNoise
        });
    }

    function setLowNoise(control, key, value) {
        const row = control.closest('.ln-row').addClass('busy');
        $.ajax({
            url: 'lownoise.php',
            method: 'POST',
            data: { key: key, value: value },
            dataType: 'json',
            timeout: 30000,
            success: showLowNoise,
            error: function(xhr) {
                const detail = (xhr.responseJSON && xhr.responseJSON.message) || '';
                customAlert(lnText('ln_error') +
                    (detail ? '<br>' + $('<div>').text(detail).html().replace(/\n/g, '<br>') : ''));
                loadLowNoise();
            },
            complete: function() {
                row.removeClass('busy');
            }
        });
    }

    $('.ln-option').click(function() {
        const button = $(this);
        if (button.hasClass('active') || button.closest('.ln-row').hasClass('busy')) {
            return;
        }
        setLowNoise(button, button.data('key'), String(button.data('value')));
    });

    $('.ln-select').change(function() {
        setLowNoise($(this), $(this).data('key'), $(this).val());
    });

    let savedTab = 'player';
    try { savedTab = localStorage.getItem('purefox_tab') || 'player'; } catch (e) { /* storage blocked */ }
    showTab($('#tab-' + savedTab).length ? savedTab : 'player');

    // Функция проверки соединения после перезагрузки
    function checkConnectionAfterReboot() {
        $.ajax({
            url: 'status_fast.php',
            method: 'GET',
            timeout: 2000,
            success: function() {
                // Соединение восстановлено, перезагружаем страницу
                location.reload();
            },
            error: function() {
                // Соединение еще не восстановлено, ждем еще
                setTimeout(checkConnectionAfterReboot, 2000);
            }
        });
    }

    // Очистка интервалов при выходе со страницы
    window.addEventListener('beforeunload', function() {
        if (statusInterval) {
            clearInterval(statusInterval);
        }
        if (statusEvents) {
            statusEvents.close();
        }
    });
    
    // Инициализация polling системы
    function startPolling() {
        forceStatusCheck();
        startStatusEvents();

        // SSE delivers state changes immediately. This is only a recovery path
        // when the event connection is temporarily unavailable.
        statusInterval = setInterval(function() {
            // Re-validate the USBtoI2S button: the mode can be toggled outside
            // this page (serial console / STM32 controller / another browser).
            checkUsbToI2sStatus();
            if (!statusEventsConnected) {
                forceStatusCheck();
            }
        }, 30000);
    }
    
    startPolling();

    // ===== USBtoI2S BUTTON FUNCTIONALITY =====
    const USBTOI2S_LOCK_KEY = 'usbToI2sLocked';

    // Function to lock ALSA toggle (USB to I2S mode)
    function lockAlsaToggle() {
        localStorage.setItem(USBTOI2S_LOCK_KEY, 'true');
        const toggleInput = $('#alsa-toggle');
        const toggleLabel = $('#alsa-toggle').next('.toggle-label');
        toggleInput.prop('disabled', true);
        toggleLabel.addClass('disabled');
        toggleLabel.css({
            'opacity': '0.4',
            'pointer-events': 'none',
            'cursor': 'not-allowed'
        });
    }

    // Function to unlock ALSA toggle
    function unlockAlsaToggle() {
        localStorage.removeItem(USBTOI2S_LOCK_KEY);
        const toggleInput = $('#alsa-toggle');
        const toggleLabel = $('#alsa-toggle').next('.toggle-label');
        toggleInput.prop('disabled', false);
        toggleLabel.removeClass('disabled');
        toggleLabel.css({
            'opacity': '',
            'pointer-events': '',
            'cursor': ''
        });
    }

    // Check USBtoI2S status and lock toggle if enabled
    function checkUsbToI2sStatus() {
        if (isAlsaSwitching) {
            // This tab's own toggle is in flight; its handlers refresh the UI.
            return;
        }
        debugLog("checkUsbToI2sStatus called");
        $.ajax({
            url: 'usb_to_i2s.php',
            method: 'POST',
            data: { action: 'status' },
            dataType: 'json',
            timeout: 3000,
            success: function(response) {
                debugLog("checkUsbToI2sStatus response:", response);
                if (response.enabled) {
                    debugLog("ADDING active class to button");
                    $('button[data-service]').removeClass('active');
                    $('#usbto-i2s-btn').addClass('active');
                    lockAlsaToggle();
                    updateAlsaUI('i2s');
                    debugLog("After addClass:", $('#usbto-i2s-btn').attr('class'));
                } else {
                    const wasActive = $('#usbto-i2s-btn').hasClass('active');
                    $('#usbto-i2s-btn').removeClass('active');
                    unlockAlsaToggle();
                    if (wasActive) {
                        // The mode was just turned off elsewhere (console/STM32,
                        // another browser): refresh the rest of the UI as well.
                        forceStatusCheck();
                    }
                }
            }
        });
    }

    // Check status on page load
    window.checkUsbToI2sStatus = checkUsbToI2sStatus;
    checkUsbToI2sStatus();

    // USBtoI2S button click handler - toggle mode
    $('#usbto-i2s-btn').click(function() {
        const isEnabled = $(this).hasClass('active');

        if (isEnabled) {
            // Disable mode
            const confirmDisable = {
                'ru': 'Отключить режим USBtoI2S?',
                'en': 'Disable USBtoI2S mode?',
                'de': 'USBtoI2S-Modus deaktivieren?',
                'fr': 'Désactiver le mode USBtoI2S?',
                'zh': '禁用USBtoI2S模式？'
            };

            customConfirm(confirmDisable[currentLang] || confirmDisable['en'], function(confirmed) {
                if (confirmed) {
                    isAlsaSwitching = true;
                    showSpinner(translations[currentLang]['switching_input']);

                    // Мгновенная деактивация кнопки
                    $('#usbto-i2s-btn').removeClass('active');

                    $.ajax({
                        url: 'usb_to_i2s.php',
                        method: 'POST',
                        data: { action: 'disable' },
                        timeout: 15000,
                        success: function() {
                            unlockAlsaToggle();
                            isAlsaSwitching = false;
                            hideSpinner();
                            forceStatusCheck();
                        },
                        error: function(xhr, status, error) {
                            isAlsaSwitching = false;
                            hideSpinner();
                            $('#usbto-i2s-btn').addClass('active');
                            console.error('USBtoI2S disable error:', status, error, 'Response:', xhr.responseText);
                            customAlert(translations[currentLang]['alsa_error']);
                        }
                    });
                }
            });
        } else {
            // Enable mode
            isAlsaSwitching = true;
            showSpinner(translations[currentLang]['switching_input']);

            // Мгновенная активация кнопки
            $('button[data-service]').removeClass('active');
            $('#usbto-i2s-btn').addClass('active');

            $.ajax({
                        url: 'usb_to_i2s.php',
                        method: 'POST',
                        data: { action: 'enable' },
                        timeout: 15000,
                        success: function() {
                            lockAlsaToggle();
                            isAlsaSwitching = false;
                            hideSpinner();
                            checkUsbToI2sStatus();
                        },
                        error: function(xhr, status, error) {
                            isAlsaSwitching = false;
                            hideSpinner();
                            $('#usbto-i2s-btn').removeClass('active');
                            console.error('USBtoI2S enable error:', status, error, 'Response:', xhr.responseText);
                            customAlert(translations[currentLang]['alsa_error']);
                        }
                    });
                }
    });

    // I2S Modal functions
    window.openI2SModal = function() {
        // Load current I2S settings
        $.ajax({
            url: 'handle_i2s.php?action=getStatus',
            method: 'GET',
            dataType: 'json',
            success: function(data) {
                // Set mode toggle
                if (data.mode === 'pll') {
                    $('#modal-mode-pll').prop('checked', true);
                } else {
                    $('#modal-mode-ext').prop('checked', true);
                }
                
                // Set MCLK toggle
                if (data.mclk === '512') {
                    $('#modal-mclk-512').prop('checked', true);
                } else {
                    $('#modal-mclk-1024').prop('checked', true);
                }
                
                // Set PCM swap toggle
                if (data.pcm_swap === '1') {
                    $('#modal-pcm-swap').prop('checked', true);
                } else {
                    $('#modal-pcm-normal').prop('checked', true);
                }
                
                // Set DSD swap toggle
                if (data.dsd_swap === '1') {
                    $('#modal-dsd-swap').prop('checked', true);
                } else {
                    $('#modal-dsd-normal').prop('checked', true);
                }
                
                // Set freq swap toggle
                if (data.freq_swap === '1') {
                    $('#modal-freq-swap').prop('checked', true);
                } else {
                    $('#modal-freq-normal').prop('checked', true);
                }
                
                // Set leftjust toggle
                if (data.leftjust === '1') {
                    $('#modal-leftjust-on').prop('checked', true);
                } else {
                    $('#modal-leftjust-off').prop('checked', true);
                }
                
                // Set submode buttons active state
                $('.i2s-submode-btn').removeClass('active');
                $(`.i2s-submode-btn[value="${data.submode}"]`).addClass('active');
                
                // Apply translations
                applyTranslationsToModal();
                
                // Show modal
                $('#i2s-modal').addClass('show');
            },
            error: function() {
                console.error('Failed to load I2S settings');
            }
        });
    };

    window.closeI2SModal = function() {
        $('#i2s-modal').removeClass('show');
    };

    window.confirmRebootI2S = function(event) {
        if (event) {
            event.preventDefault();
        }
        const currentLang = detectLanguage();
        const rebootMessages = {
            'ru': 'Применить настройки I2S и перезагрузить устройство?',
            'en': 'Apply I2S settings and reboot the device?',
            'de': 'I2S-Einstellungen anwenden und Gerät neu starten?',
            'fr': 'Appliquer les paramètres I2S et redémarrer l\'appareil?',
            'zh': '应用I2S设置并重启设备？'
        };
        const message = rebootMessages[currentLang] || rebootMessages['en'];
        
        customConfirm(message, function(confirmed) {
            if (confirmed) {
                applyI2SSettings();
            }
        });
        return false;
    };

    function applyI2SSettings() {
        const formData = new FormData();
        
        // Get selected mode
        const mode = $('input[name="mode"]:checked').val();
        if (mode) formData.append('mode', mode);
        
        // Get selected MCLK
        const mclk = $('input[name="mclk"]:checked').val();
        if (mclk) formData.append('mclk', mclk);
        
        // Get selected submode
        const activeSubmodeBtn = $('.i2s-submode-btn.active');
        if (activeSubmodeBtn.length > 0) {
            formData.append('submode', activeSubmodeBtn.attr('value'));
        }
        
        // Show spinner
        $('.spinner-overlay').addClass('show');
        const currentLang = detectLanguage();
        const rebootingTexts = {
            'ru': 'Применение настроек и перезагрузка...',
            'en': 'Applying settings and rebooting...',
            'de': 'Anwenden der Einstellungen und Neustart...',
            'fr': 'Application des paramètres et redémarrage...',
            'zh': '应用设置并重启中...'
        };
        $('.spinner-text').text(rebootingTexts[currentLang] || rebootingTexts['en']);
        
        // Close modal first
        closeI2SModal();
        
        // Apply settings
        fetch('handle_i2s.php', {
            method: 'POST',
            body: formData
        }).then(() => {
            // Reboot after applying settings
            return fetch('reboot.php', { method: 'POST' });
        }).then(() => {
            setTimeout(checkConnectionAfterReboot, 3000);
        }).catch(() => {
            setTimeout(checkConnectionAfterReboot, 3000);
        });
    }

    function applyTranslationsToModal() {
        const currentLang = detectLanguage();
        const translations = window.translations[currentLang] || window.translations['en'];
        
        $('#i2s-modal [data-lang]').each(function() {
            const key = $(this).data('lang');
            if (translations[key]) {
                $(this).text(translations[key]);
            }
        });
    }

    // I2S Modal event handlers - submit submode immediately like in original i2s.php
    $(document).on('click', '.i2s-submode-btn', function() {
        $('.i2s-submode-btn').removeClass('active');
        $(this).addClass('active');
        
        const formData = new FormData();
        formData.append('submode', $(this).attr('value'));
        
        // Apply submode setting immediately
        fetch('handle_i2s.php', {
            method: 'POST',
            body: formData
        }).then(() => {
            debugLog(`Applied submode: ${$(this).attr('value')}`);
        }).catch(error => {
            console.error('Error applying submode:', error);
        });
    });
    
    // Handle toggle switches in modal - submit immediately like in original i2s.php
    $(document).on('change', '#i2s-modal .toggle-input-compact', function() {
        if (this.checked) {
            const formData = new FormData();
            
            if (this.name === 'mode') {
                formData.append('mode', this.value);
            } else if (this.name === 'mclk') {
                formData.append('mclk', this.value);
            } else if (this.name === 'pcm_swap') {
                formData.append('pcm_swap', this.value);
            } else if (this.name === 'dsd_swap') {
                formData.append('dsd_swap', this.value);
            } else if (this.name === 'freq_swap') {
                formData.append('freq_swap', this.value);
            } else if (this.name === 'leftjust') {
                formData.append('leftjust', this.value);
            }
            
            // Apply setting immediately
            fetch('handle_i2s.php', {
                method: 'POST',
                body: formData
            }).then(() => {
                // Settings applied successfully
                debugLog(`Applied ${this.name}: ${this.value}`);
            }).catch(error => {
                console.error('Error applying setting:', error);
            });
        }
    });

    // Prevent modal from closing when clicking inside modal content
    $(document).on('click', '#i2s-modal .modal-content', function(e) {
        e.stopPropagation();
    });
    
    // Close modal when clicking outside
    $(document).on('click', '#i2s-modal', function() {
        closeI2SModal();
    });

    // I2S settings link handler
    $(document).on('click', '#i2s-settings-link', function(e) {
        e.preventDefault();
        openI2SModal();
        return false;
    });

    // ===== SWIPE TO HIDE BUTTONS FUNCTIONALITY =====
    const HIDDEN_BUTTONS_KEY = 'hiddenButtons';
    let startX = 0;
    let endX = 0;
    const SWIPE_THRESHOLD = 100;

    // Load hidden buttons from localStorage on page load
    function loadHiddenButtons() {
        const hidden = localStorage.getItem(HIDDEN_BUTTONS_KEY);
        if (hidden) {
            try {
                const hiddenArray = JSON.parse(hidden);
                hiddenArray.forEach(id => {
                    if (id.startsWith('#')) {
                        $(id).hide();
                    } else {
                        $(`button[data-service="${id}"]`).hide();
                    }
                });
            } catch (e) {
                console.error('Failed to parse hidden buttons:', e);
            }
        }
    }

    // Save hidden buttons to localStorage
    function saveHiddenButtons() {
        const hiddenButtons = [];
        $('button[data-service]').each(function() {
            if ($(this).is(':hidden')) {
                hiddenButtons.push($(this).data('service'));
            }
        });
        ['#usbto-i2s-btn', '#dlna-bridge-btn'].forEach(id => {
            if ($(id).is(':hidden')) hiddenButtons.push(id);
        });
        localStorage.setItem(HIDDEN_BUTTONS_KEY, JSON.stringify(hiddenButtons));
    }

    // Handle swipe gestures on player buttons (touch and mouse)
    $('button[data-service], #usbto-i2s-btn, #dlna-bridge-btn').each(function() {
        const $button = $(this);
        let isDragging = false;
        
        // Touch events
        this.addEventListener('touchstart', function(e) {
            startX = e.changedTouches[0].screenX;
        }, { passive: true });

        this.addEventListener('touchend', function(e) {
            endX = e.changedTouches[0].screenX;
            handleSwipe($button);
        }, { passive: true });

        // Mouse events for desktop
        $button.on('mousedown', function(e) {
            startX = e.screenX;
            isDragging = false;
        });

        $button.on('mousemove', function(e) {
            if (e.buttons === 1) { // Left mouse button is pressed
                isDragging = true;
            }
        });

        $button.on('mouseup', function(e) {
            if (isDragging) {
                endX = e.screenX;
                handleSwipe($button);
                e.preventDefault();
                e.stopPropagation();
            }
            isDragging = false;
        });
    });

    function handleSwipe($button) {
        const swipeDistance = endX - startX;
        
        // Swipe left to hide
        if (swipeDistance < -SWIPE_THRESHOLD) {
            // Suppress the click event that follows touch-end / mouse-up
            // so the player is not switched to the one being hidden.
            window._purefoxSwipeInProgress = true;
            const serviceName = $button.text().trim();
            const currentHost = window.location.hostname;
            const resetUrl = `http://${currentHost}/default.php`;
            
            const hideText = {
                'ru': 'Скрыть кнопку',
                'en': 'Hide button',
                'de': 'Schaltfläche ausblenden',
                'fr': 'Masquer le bouton',
                'zh': '隐藏按钮'
            };
            
            const restoreText = {
                'ru': 'Для восстановления всех кнопок откройте:',
                'en': 'To restore all buttons, visit:',
                'de': 'Um alle Schaltflächen wiederherzustellen, besuchen Sie:',
                'fr': 'Pour restaurer tous les boutons, visitez:',
                'zh': '要恢复所有按钮，请访问：'
            };
            
            const message = `${hideText[currentLang] || hideText['en']} "${serviceName}"?<br><br>${restoreText[currentLang] || restoreText['en']}<br>${resetUrl}`;
            
            customConfirm(message, function(confirmed) {
                if (confirmed) {
                    const buttonHeight = $button.outerHeight(true);
                    
                    // Шаг 1: Фиксируем высоту БЕЗ transition
                    $button.css({
                        'height': buttonHeight + 'px',
                        'overflow': 'hidden'
                    });
                    
                    // Шаг 2: Через 20ms добавляем transition и запускаем обе анимации
                    setTimeout(function() {
                        $button.css({
                            'transition': 'height 1s ease-in-out, margin 1s ease-in-out, opacity 1s ease, transform 1s ease, filter 1s ease'
                        });
                        
                        // Шаг 3: Через 20ms запускаем распыление и схлопывание одновременно
                        setTimeout(function() {
                            $button.addClass('btn-dissolving');
                            $button.css({
                                'height': '0',
                                'margin-top': '0',
                                'margin-bottom': '0'
                            });
                            
                            setTimeout(function() {
                                $button.hide();
                                saveHiddenButtons();
                                checkAndExpandButtons();
                            }, 1000);
                        }, 20);
                    }, 20);
                }
            });
        }
    }

    // Check if button pairs need to expand to full width
    function checkAndExpandButtons() {
        // Only on desktop (width > 500px)
        if (window.innerWidth <= 500) return;
        
        // Check each player-buttons container individually
        $('.player-buttons').each(function() {
            const $visible = $(this).find('button[data-service]:visible');
            if ($visible.length === 1) {
                $visible.css('width', '100%');
            }
        });
        
        // Check each streaming-buttons container individually
        $('.streaming-buttons').each(function() {
            const $visible = $(this).find('button[data-service]:visible');
            if ($visible.length === 1) {
                $visible.css('width', '100%');
            }
        });
    }

    // Load hidden buttons on page load
    loadHiddenButtons();
    checkAndExpandButtons();

    // ===== DLNA BRIDGE FUNCTIONALITY =====

    let dlnaRenderers = [];         // discovered renderer list
    let dlnaSelected  = null;       // currently selected renderer object

    function initDlnaBridge() {
        // Restore button state immediately from localStorage to prevent flash on refresh
        if (localStorage.getItem('dlna_bridge_active') === 'true') {
            isDlnaBridgeActive = true;
            $('#dlna-bridge-btn').addClass('active');
        }
        $.ajax({
            url: 'handle_dlna.php?action=status',
            method: 'GET',
            timeout: 5000,
            dataType: 'json',
            success: function(data) {
                if (data.bridge_state === 'enabled') {
                    isDlnaBridgeActive = true;
                    localStorage.setItem('dlna_bridge_active', 'true');
                    $('#dlna-bridge-btn').addClass('active');
                } else {
                    isDlnaBridgeActive = false;
                    localStorage.removeItem('dlna_bridge_active');
                    $('#dlna-bridge-btn').removeClass('active');
                }
                updateDlnaStreamInfo(data.stream);
                if (data.renderer_ip) {
                    dlnaSelected = {
                        ip:          data.renderer_ip,
                        port:        data.renderer_port,
                        control_url: ''
                    };
                    $('#dlna-manual-ip').val(data.renderer_ip);
                    $('#dlna-manual-port').val(data.renderer_port);
                }
            },
            error: function() {
                debugWarn('DLNA bridge status unavailable');
                // Keep localStorage state — bridge may still be running
            }
        });
    }

    function updateDlnaStreamInfo(stream) {
        if (!stream || !stream.active) {
            $('#dlna-stream-info').text('Stream: inactive');
            return;
        }
        $('#dlna-stream-info').text(
            'Stream: ' + stream.rate + ' Hz / ' + stream.bits + '-bit / ' +
            stream.channels + 'ch  —  clients: ' + stream.clients
        );
    }

    // Settings modal — open (called from inline onclick in HTML)
    window.openDlnaModal = function() {
        $('#dlna-modal').addClass('show');
        $.ajax({
            url: 'handle_dlna.php?action=status',
            method: 'GET',
            timeout: 3000,
            dataType: 'json',
            success: function(data) {
                updateDlnaStreamInfo(data.stream);
                if (data.renderer_ip) {
                    $('#dlna-manual-ip').val(data.renderer_ip);
                    $('#dlna-manual-port').val(data.renderer_port);
                }
            }
        });
    };

    // Close modal
    $('#dlna-modal-close').click(function() {
        $('#dlna-modal').removeClass('show');
    });
    $('#dlna-modal').click(function(e) {
        if (e.target === this) $('#dlna-modal').removeClass('show');
    });
    $('#dlna-modal .modal-content').click(function(e) { e.stopPropagation(); });

    // Discover button
    $('#dlna-discover-btn').click(function() {
        $('#dlna-renderer-list').html('<em>Discovering...</em>');
        $.ajax({
            url: 'handle_dlna.php?action=discover',
            method: 'GET',
            timeout: 8000,
            dataType: 'json',
            success: function(renderers) {
                dlnaRenderers = renderers;
                if (!renderers || renderers.length === 0) {
                    $('#dlna-renderer-list').html('<em>No renderers found</em>');
                    return;
                }
                var html = '<strong>Found renderers:</strong><ul style="list-style:none;padding:0;margin:4px 0">';
                renderers.forEach(function(r, i) {
                    html += '<li style="cursor:pointer;padding:4px 6px;border-radius:4px" ' +
                            'class="dlna-renderer-item" data-idx="' + i + '">' +
                            r.name + ' (' + r.ip + ':' + r.port + ')</li>';
                });
                html += '</ul>';
                $('#dlna-renderer-list').html(html);

                // Click to select
                $('.dlna-renderer-item').click(function() {
                    var idx = parseInt($(this).data('idx'));
                    dlnaSelected = dlnaRenderers[idx];
                    $('#dlna-manual-ip').val(dlnaSelected.ip);
                    $('#dlna-manual-port').val(dlnaSelected.port);
                    $('#dlna-manual-url').val(dlnaSelected.control_url);
                    $('.dlna-renderer-item').css('background', '');
                    $(this).css('background', '#2a4a2a');
                });
            },
            error: function() {
                $('#dlna-renderer-list').html('<em>Discovery failed</em>');
            }
        });
    });

    // Push button
    $('#dlna-push-btn').click(function() {
        var ip   = $('#dlna-manual-ip').val().trim();
        var port = parseInt($('#dlna-manual-port').val().trim()) || 0;
        var url  = $('#dlna-manual-url').val().trim();

        if (!ip || !port) {
            customAlert('Please enter renderer IP and port (or use Discover)');
            return;
        }

        // Save renderer config then push
        $.ajax({
            url: 'handle_dlna.php',
            method: 'POST',
            data: { action: 'setrenderer', renderer_ip: ip, renderer_port: port, control_url: url },
            timeout: 5000,
            dataType: 'json',
            success: function() {
                $.ajax({
                    url: 'handle_dlna.php',
                    method: 'POST',
                    data: { action: 'push' },
                    timeout: 5000,
                    dataType: 'json',
                    success: function(resp) {
                        if (resp.success) {
                            customAlert('Stream pushed to renderer');
                        } else {
                            customAlert('Push failed: ' + (resp.error || 'unknown'));
                        }
                    },
                    error: function() { customAlert('Push request failed'); }
                });
            },
            error: function() { customAlert('Failed to save renderer settings'); }
        });
    });

    // Init on page load
    initDlnaBridge();

});
