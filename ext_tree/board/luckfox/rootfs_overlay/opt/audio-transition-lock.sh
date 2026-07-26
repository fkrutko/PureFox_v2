#!/bin/sh

LOCK_FILE=/tmp/purefox-audio.lock
LOCK_FD_HELD=0

if [ "${PUREFOX_AUDIO_LOCK_HELD:-0}" != "1" ]; then
    exec 9>"$LOCK_FILE"
    flock 9
    LOCK_FD_HELD=1
fi

run_without_audio_lock_fd() {
    if [ "$LOCK_FD_HELD" -eq 1 ]; then
        (
            exec 9>&-
            "$@"
        )
    else
        "$@"
    fi
}
