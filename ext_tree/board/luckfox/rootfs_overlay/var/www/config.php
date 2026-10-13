<?php
define('PUREFOX_BASE_VERSION', '2.8');
// Full build id (e.g. 2.8.12), written at build time by post-build.sh
$purefox_build = trim((string)@file_get_contents('/etc/purefox-release'));
define('PUREFOX_VERSION', ($purefox_build !== '' ? $purefox_build : PUREFOX_BASE_VERSION));
define('VERSION', PUREFOX_VERSION);


