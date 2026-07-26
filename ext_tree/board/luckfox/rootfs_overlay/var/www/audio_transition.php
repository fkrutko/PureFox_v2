<?php

const PUREFOX_AUDIO_LOCK_FILE = '/tmp/purefox-audio.lock';

function acquireAudioTransitionLock() {
    $lock = fopen(PUREFOX_AUDIO_LOCK_FILE, 'ce');
    if ($lock === false) {
        return false;
    }

    if (!flock($lock, LOCK_EX | LOCK_NB)) {
        fclose($lock);
        return false;
    }

    return $lock;
}

function releaseAudioTransitionLock($lock) {
    if (is_resource($lock)) {
        flock($lock, LOCK_UN);
        fclose($lock);
    }
}
