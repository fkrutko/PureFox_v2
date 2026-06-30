<?php
header('Content-Type: text/plain');

$player = $_GET['player'] ?? '';

$allowed = ['aplayer', 'aprenderer', 'apscream', 'all', 'list'];
if (!in_array($player, $allowed, true)) {
    http_response_code(400);
    echo "Invalid player. Use: aplayer, aprenderer, apscream, all, list\n";
    exit;
}

$cmd = "/usr/bin/sudo /opt/update_ap.sh " . escapeshellarg($player) . " 2>&1";
$descriptorspec = [
    1 => ["pipe", "w"],
    2 => ["pipe", "w"]
];

$process = proc_open($cmd, $descriptorspec, $pipes);

if (is_resource($process)) {
    while ($line = fgets($pipes[1])) {
        echo $line;
        ob_flush();
        flush();
    }
    fclose($pipes[1]);
    fclose($pipes[2]);
    proc_close($process);
}
