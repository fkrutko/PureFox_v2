<?php
require_once 'audio_transition.php';

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $action = $_POST['action'] ?? '';
    $audioLock = null;

    if ($action === 'enable' || $action === 'disable') {
        $audioLock = acquireAudioTransitionLock();
        if (!$audioLock) {
            http_response_code(409);
            echo 'Audio transition already in progress';
            exit;
        }
    }

    try {
        // Clear cache before publishing a new transition result.
        $cache_file = '/tmp/combined_status_cache';
        if (file_exists($cache_file)) {
            unlink($cache_file);
        }

        if ($action === 'enable') {
            $script = '/opt/usb_to_i2s.sh';
            $successMessage = 'USBtoI2S mode enabled successfully';
        } elseif ($action === 'disable') {
            $script = '/opt/usb_unlock.sh';
            $successMessage = 'USBtoI2S mode disabled successfully';
        } elseif ($action === 'status') {
            echo json_encode(['enabled' => file_exists('/etc/usb_to_i2s.state')]);
            $script = null;
        } else {
            http_response_code(400);
            echo 'Invalid action. Allowed values: enable, disable, status';
            $script = null;
        }

        if ($script !== null) {
            if (!file_exists($script)) {
                throw new RuntimeException("Script $script not found", 500);
            }

            $output = [];
            $returnVar = 0;
            exec('PUREFOX_AUDIO_LOCK_HELD=1 ' . escapeshellcmd($script) . ' 2>&1', $output, $returnVar);
            if ($returnVar !== 0) {
                throw new RuntimeException(implode("\n", $output), 500);
            }

            echo $successMessage;
        }
    } catch (RuntimeException $error) {
        http_response_code($error->getCode() ?: 500);
        echo 'Error: ' . $error->getMessage();
    } finally {
        releaseAudioTransitionLock($audioLock);
    }
} else {
    http_response_code(405);
    header('Allow: POST');
    echo 'Use POST request';
}
?>
