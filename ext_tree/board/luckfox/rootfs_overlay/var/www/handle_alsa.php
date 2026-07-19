<?php

if ($_SERVER['REQUEST_METHOD'] === 'POST') {
    $targetCard = $_POST['card'] ?? '';
    
    // Input validation
    if (!in_array($targetCard, ['usb', 'i2s'])) {
        http_response_code(400);
        die('Invalid card parameter. Allowed values: usb, i2s');
    }

    // Clear cache
    $cache_file = '/tmp/combined_status_cache';
    if (file_exists($cache_file)) {
        unlink($cache_file);
    }

    // Call corresponding script
    $script = ($targetCard === 'usb') ? '/opt/2_usb.sh' : '/opt/2_i2s.sh';
    if (!file_exists($script)) {
        http_response_code(500);
        die("Script $script not found");
    }

    // Execute script
    $output = [];
    $returnVar = 0;
    exec(escapeshellcmd($script) . ' 2>&1', $output, $returnVar);

    // Don't fail if script exits with error (service may not exist)
    if ($returnVar !== 0) {
        error_log("Script $script exited with code $returnVar: " . implode("\n", $output));
        // Continue execution, don't interrupt
    }

    // Output switching changes the active ALSA card and mixer control.
    // Restart the monitor so its control cache and event descriptors match it
    // before it publishes the next status update to the web UI.
    shell_exec('/etc/init.d/S40statusmonitor restart 2>/dev/null');

    // Additional service restart (if exists)
    $serviceOutput = shell_exec('/bin/sh -c "/etc/init.d/S95* restart" 2>/dev/null');
    
    // Clear cache after switching
    if (file_exists($cache_file)) {
        unlink($cache_file);
    }
    
    echo "Switched to $targetCard successfully";
} else {
    http_response_code(405);
    header('Allow: POST');
    echo 'Use POST request';
}
?>
