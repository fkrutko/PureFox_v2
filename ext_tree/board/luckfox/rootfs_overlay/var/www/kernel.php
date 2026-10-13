<?php
// Linux kernel selection, see /opt/kernel.sh: 6.1 (default), 5.10, or 1.6
// (5.10 with the PureFox 1.6 I2S driver).
// GET: status. POST kernel=6.1|5.10|1.6: write that kernel and reboot.
header('Content-Type: application/json');

const KERNELS = ['6.1', '5.10', '1.6'];

function kernelStatus() {
    $release = php_uname('r');
    if (substr($release, -5) === '-pf16') {
        $running = '1.6';
    } elseif (strpos($release, '5.10.') === 0) {
        $running = '5.10';
    } else {
        $running = '6.1';
    }
    $selected = trim((string) @file_get_contents('/etc/kernel'));
    if (!in_array($selected, KERNELS, true)) {
        $selected = '6.1';
    }
    $available = ['6.1'];
    foreach (['5.10', '1.6'] as $kernel) {
        if (file_exists("/data/boot/$kernel/zImage")) {
            $available[] = $kernel;
        }
    }
    return [
        'running' => $running,
        'selected' => $selected,
        'available' => $available,
        'usb_to_i2s' => file_exists('/etc/usb_to_i2s.state'),
    ];
}

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    echo json_encode(kernelStatus());
    exit;
}

$kernel = $_POST['kernel'] ?? '';
if (!in_array($kernel, KERNELS, true)) {
    http_response_code(400);
    echo json_encode(['ok' => false, 'message' => 'Invalid kernel']);
    exit;
}

exec('/usr/bin/sudo /opt/kernel.sh ' . escapeshellarg($kernel) . ' 2>&1', $output, $returnVar);
if ($returnVar !== 0) {
    http_response_code(500);
    echo json_encode(['ok' => false, 'message' => implode("\n", $output)]);
    exit;
}

exec('/usr/bin/sudo /opt/reboot.sh > /dev/null 2>&1 &');
echo json_encode(['ok' => true, 'message' => implode("\n", $output)]);
