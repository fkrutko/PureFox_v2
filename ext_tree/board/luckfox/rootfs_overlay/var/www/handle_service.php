<?php
require_once 'audio_transition.php';

header('Content-Type: application/json; charset=utf-8');
error_reporting(E_ALL);
ini_set('display_errors', 1);

function logMessage($message) {
    error_log("[Player Manager] " . $message);
}

function executeCommand($command) {
    logMessage("Executing: $command");
    $command = 'PUREFOX_AUDIO_LOCK_HELD=1 ' . $command;
    $output = shell_exec("/usr/bin/sudo /bin/sh -c " . escapeshellarg($command) . " 2>&1");
    logMessage("Output: " . trim((string)$output));
    return trim((string)$output);
}

function fail($message) {
    logMessage("Error: $message");
    echo json_encode(['status' => 'error', 'message' => $message]);
    exit;
}

function stopProcessGroup($processList) {
    $plist = implode(' ', array_map('escapeshellarg', $processList));
    executeCommand("killall $plist 2>/dev/null || true");
    executeCommand("sleep 0.1");
    executeCommand("killall -9 $plist 2>/dev/null || true");
}

function waitForProcess($process, $timeoutMs = 1000) {
    $attempts = (int)($timeoutMs / 100);
    for ($attempt = 0; $attempt < $attempts; $attempt++) {
        $pid = trim((string)shell_exec('/bin/pidof ' . escapeshellarg($process) . ' 2>/dev/null'));
        if ($pid !== '') {
            return true;
        }
        usleep(100000);
    }
    return false;
}

$players = [
    'naa'        => ['process' => 'networkaudiod',   'script' => 'S95naa'],
    'scream'     => ['process' => 'scream',          'script' => 'S95scream'],
    'apscream'   => ['process' => 'apscream',        'script' => 'S95apscream'],
    'aprenderer' => ['process' => 'ap2renderer',     'script' => 'S95aprenderer'],
];

if ($_SERVER['REQUEST_METHOD'] !== 'POST') {
    fail("Invalid request method");
}

$playerToStart = $_POST['service'] ?? '';
if (!isset($players[$playerToStart])) {
    fail("Invalid player: $playerToStart");
}

$scriptPath = "/etc/rc.pure/{$players[$playerToStart]['script']}";
if (!file_exists($scriptPath)) {
    fail("Player script not found: $scriptPath");
}

$lockFp = acquireAudioTransitionLock();
if (!$lockFp) {
    fail("Audio transition already in progress");
}

try {
    logMessage("Request to start player: $playerToStart");

    if (file_exists('/etc/usb_to_i2s.state')) {
        executeCommand('/opt/usb_unlock.sh');
    }

    // Stop previously active managed player if present
    executeCommand('[ -x /etc/init.d/S95player ] && /etc/init.d/S95player stop || true');

    // Hard-stop all known player processes (fallback)
    stopProcessGroup(['networkaudiod', 'scream', 'apscream', 'ap2renderer']);

    // Create one stable managed symlink. Older firmware left player links
    // under other names (S95naa): drop every link to a player script so only
    // one player starts at boot, other S95 services stay.
    $playerScripts = array_map(function ($p) {
        return '/etc/rc.pure/' . $p['script'];
    }, $players);
    foreach (glob('/etc/init.d/S95*') as $link) {
        if (is_link($link) && in_array(readlink($link), $playerScripts, true)) {
            executeCommand('rm -f ' . escapeshellarg($link));
        }
    }
    executeCommand('rm -f /etc/init.d/S95player');
    executeCommand('ln -s ' . escapeshellarg($scriptPath) . ' /etc/init.d/S95player');
    // Flush the choice to flash so it survives a power cut
    executeCommand('sync');

    // Start selected player in foreground of this shell call (script itself backgrounds daemons as needed)
    $startOutput = executeCommand('/etc/init.d/S95player start');

    // Publish the transition immediately. status_monitor verifies the actual
    // process state and pushes it to connected browsers through SSE.
    executeCommand('/opt/dbus_notify ServiceChanged ' . escapeshellarg($playerToStart) . ' 2>/dev/null || true');

    // Confirm the actual process without a fixed one-second delay.
    $proc = $players[$playerToStart]['process'];
    $confirmed = waitForProcess($proc, 1000);

    echo json_encode([
        'status' => $confirmed ? 'success' : 'error',
        'message' => $confirmed ? "Switched to $playerToStart" : "Failed to start $playerToStart",
        'confirmed' => $confirmed,
        'output' => $startOutput
    ]);
} finally {
    releaseAudioTransitionLock($lockFp);
}
?>
