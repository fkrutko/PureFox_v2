<?php
// Low noise tab, see /opt/lownoise.sh.
// GET: settings and what this kernel supports.
// POST key=..., value=...: store one setting and apply it.
header('Content-Type: application/json');

const LOWNOISE_CONF = '/etc/lownoise.conf';
const CPUFREQ = '/sys/devices/system/cpu/cpu0/cpufreq';
const DMA_PARAM = '/sys/module/snd_soc_rockchip_i2s_tdm/parameters/periods_min';
const I2S_CONF = '/etc/i2s.conf';
const DEFAULTS = [
    'CPU_MAX_FREQ' => '0',
    'I2S_DRIVE' => 'default',
    'MCLK_DRIVE' => 'default',
    'DMA_PERIODS' => '16',
    'SCREAM_RESAMPLE' => '1',
];

function readSettings() {
    $settings = DEFAULTS;
    foreach (@file(LOWNOISE_CONF, FILE_IGNORE_NEW_LINES) ?: [] as $line) {
        if (preg_match('/^([A-Z0-9_]+)=(.*)$/', $line, $m) && isset($settings[$m[1]])) {
            $settings[$m[1]] = trim($m[2]);
        }
    }
    return $settings;
}

function sysfs($path) {
    return trim((string) @file_get_contents($path));
}

// PLL or EXT; EXT when unset, as in handle_i2s.php
function i2sMode() {
    $conf = (string) @file_get_contents(I2S_CONF);
    return preg_match('/^MODE=(pll)\b/mi', $conf) ? 'pll' : 'ext';
}

function lownoiseStatus() {
    $frequencies = array_values(array_filter(preg_split('/\s+/', sysfs(CPUFREQ . '/scaling_available_frequencies'))));
    return [
        'settings' => readSettings(),
        'cpu' => [
            'frequencies' => array_map('intval', $frequencies),
            'current' => (int) sysfs(CPUFREQ . '/scaling_cur_freq'),
        ],
        'dma_supported' => file_exists(DMA_PARAM),
        'i2s_mode' => i2sMode(),
    ];
}

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    echo json_encode(lownoiseStatus());
    exit;
}

$key = $_POST['key'] ?? '';
$value = $_POST['value'] ?? '';
if (!array_key_exists($key, DEFAULTS) || !preg_match('/^[a-z0-9]+$/', $value)) {
    http_response_code(400);
    echo json_encode(['ok' => false, 'message' => 'Invalid setting']);
    exit;
}

exec('/usr/bin/sudo /opt/lownoise.sh set ' . escapeshellarg($key) . ' ' . escapeshellarg($value) . ' 2>&1', $output, $returnVar);
if ($returnVar !== 0) {
    http_response_code(500);
    echo json_encode(['ok' => false, 'message' => implode("\n", $output)]);
    exit;
}

echo json_encode(['ok' => true] + lownoiseStatus());
