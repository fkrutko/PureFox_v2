#!/bin/sh

set -ve

[ "$#" -ge 1 ] || {
    echo "Usage: $0 <target-dir>" >&2
    exit 1
}

TARGET_DIR=$1

chown root:root "$TARGET_DIR/usr/bin/php-cgi"
chmod u+s "$TARGET_DIR/usr/bin/php-cgi"
