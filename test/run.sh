#!/usr/bin/env sh
# Host-side unit tests for the pure data-plane logic (no ESP-IDF required).
set -eu
cd "$(dirname "$0")/.."

CC=${CC:-cc}
$CC -std=c11 -Wall -Wextra -Werror -O1 -g \
    -Icomponents/bridge/include \
    components/bridge/bridge_ring.c \
    components/bridge/bridge_read.c \
    test/ring_test.c \
    -o build-host/ring_test

./build-host/ring_test