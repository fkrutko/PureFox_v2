<?php
header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store, no-cache, must-revalidate, max-age=0');
header('Pragma: no-cache');
header('Expires: 0');

// CORRECT JSON reading from C-monitor
$status_file = '/tmp/system_status.json';

// First try to read file from C-monitor
if (file_exists($status_file)) {
    $age = time() - filemtime($status_file);
    
    // Allow cache to be up to 60 seconds old
    if ($age < 60) {
        $content = file_get_contents($status_file);
        if ($content) {
            $decoded = json_decode($content, true);
            if ($decoded && is_array($decoded)) {
                // Check that all required fields are present
                $has_fields = isset($decoded['active_service'], $decoded['alsa_state'], $decoded['usb_dac']);
                if (!$has_fields) {
                    error_log("status_fast.php: Missing fields in JSON: " . print_r($decoded, true));
                }
                if ($has_fields) {
                    $boot_id = trim((string) @file_get_contents('/proc/sys/kernel/random/boot_id'));
                    if ($boot_id !== '') {
                        $decoded['boot_id'] = $boot_id;
                    }

                    // Add marker that data is from C-monitor
                    $decoded['source'] = 'c_monitor';
                    echo json_encode($decoded);
                    exit;
                }
            }
        }
    }
}

// Fallback - if C-monitor not working, use old PHP approach
$services = [
    'naa'         => 'networkaudiod',
    'scream'      => 'scream',
    'apscream'    => 'apscream',
    'aprenderer'  => 'ap2renderer',
    'mpd'         => 'mpd',
];

$status = [
    'active_service' => '',
    'alsa_state' => 'unknown',
    'usb_dac' => false,
    'source' => 'php_fallback'
];

function findUsbDacCard() {
    foreach (glob('/sys/class/sound/card*/device') as $devicePath) {
        $resolvedPath = realpath($devicePath);
        if ($resolvedPath === false || strpos($resolvedPath, '/usb') === false) {
            continue;
        }

        $cardName = basename(dirname($devicePath));
        return ctype_digit(substr($cardName, 4)) ? (int)substr($cardName, 4) : null;
    }

    return null;
}

$boot_id = trim((string) @file_get_contents('/proc/sys/kernel/random/boot_id'));
if ($boot_id !== '') {
    $status['boot_id'] = $boot_id;
}

// Determine active service
$all_processes = implode('|', array_values($services));
$output = shell_exec("ps -eo comm | grep -E '^($all_processes)$' | head -1");

if (!empty($output)) {
    $found_process = trim($output);
    foreach ($services as $key => $process) {
        if ($found_process === $process) {
            $status['active_service'] = $key;
            break;
        }
    }
}

// ALSA state check
$output_file = '/etc/output';
if (file_exists($output_file)) {
    $output_content = trim(file_get_contents($output_file));
    if (strtoupper($output_content) === 'USB') {
        $status['alsa_state'] = 'usb';
    } elseif (strtoupper($output_content) === 'I2S') {
        $status['alsa_state'] = 'i2s';
    }
}

// USB DAC check
$status['usb_dac'] = findUsbDacCard() !== null;

echo json_encode($status);
?>
