<?php
header('Content-Type: text/plain');

// Open process and get real stdout stream from script execution
// ?target=ppy: back to the upstream firmware of ppy2 (/opt/to_ppy.sh)
$script = (($_GET['target'] ?? '') === 'ppy') ? '/opt/to_ppy.sh' : '/opt/update.sh';
$cmd = "/usr/bin/sudo $script 2>&1";
$descriptorspec = array(
    1 => array("pipe", "w"), // stdout
    2 => array("pipe", "w")  // stderr
);

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
?>

