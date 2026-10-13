<?php
// Linux kernel selection: 6.1 (default) or 5.10, see /opt/kernel.sh.
// GET: status. POST kernel=6.1|5.10: write that kernel and reboot.
header('Content-Type: application/json');

function kernelStatus() {
    return [
        'running' => strpos(php_uname('r'), '5.10.') === 0 ? '5.10' : '6.1',
        'selected' => trim((string) @file_get_contents('/etc/kernel')) === '5.10' ? '5.10' : '6.1',
        'available' => file_exists('/data/boot/5.10/zImage'),
        'usb_to_i2s' => file_exists('/etc/usb_to_i2s.state'),
    ];
}

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    echo json_encode(kernelStatus());
    exit;
}

$kernel = $_POST['kernel'] ?? '';
if (!in_array($kernel, ['6.1', '5.10'], true)) {
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
