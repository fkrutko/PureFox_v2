<?php
require_once 'audio_transition.php';

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $targetCard = $_POST['card'] ?? '';
    
    // Input validation
    if (!in_array($targetCard, ['usb', 'i2s'])) {
        http_response_code(400);
        die('Invalid card parameter. Allowed values: usb, i2s');
    }

    // Call corresponding script
    $script = ($targetCard === 'usb') ? '/opt/2_usb.sh' : '/opt/2_i2s.sh';
    if (!file_exists($script)) {
        http_response_code(500);
        die("Script $script not found");
    }

    $lockFp = acquireAudioTransitionLock();
    if (!$lockFp) {
        http_response_code(409);
        echo 'Audio transition already in progress';
        exit;
    }

    try {
        $cache_file = '/tmp/combined_status_cache';
        if (file_exists($cache_file)) {
            unlink($cache_file);
        }

        // The transition script owns player and status-monitor restarts.
        $output = [];
        $returnVar = 0;
        exec('PUREFOX_AUDIO_LOCK_HELD=1 ' . escapeshellcmd($script) . ' 2>&1', $output, $returnVar);

        if ($returnVar !== 0) {
            error_log("Script $script exited with code $returnVar: " . implode("\n", $output));
            http_response_code(500);
            echo "Output switch failed: " . implode("\n", $output);
        } else {
            if (file_exists($cache_file)) {
                unlink($cache_file);
            }

            echo "Switched to $targetCard successfully";
        }
    } finally {
        releaseAudioTransitionLock($lockFp);
    }
} else {
    http_response_code(405);
    header('Allow: POST');
    echo 'Use POST request';
}
?>
