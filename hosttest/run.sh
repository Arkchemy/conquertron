#!/bin/sh
# Build and run the host TLSF harness. No Switch, no NRO, no game.
set -e
cd "$(dirname "$0")/../.."
OUT=${OUT:-/tmp/tlsf_harness}
gcc -O1 -g -w \
    -I conquertron/include -I jouster/game/include \
    conquertron/hosttest/tlsf_harness.c \
    conquertron/hosttest/stubs.c \
    jouster/game/source/generated_0166.c \
    -o "$OUT" -lm
exec "$OUT"
