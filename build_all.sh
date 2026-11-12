#!/bin/sh
# Builds the CRSF relay firmware into dist/
set -e
: "${PICO_SDK_PATH:?PICO_SDK_PATH muss auf das Pico SDK (2.x) zeigen}"
VERSION=1.1.0
mkdir -p dist
cmake -G Ninja -B build/rp2040-zero -DPICO_BOARD=waveshare_rp2040_zero -DCMAKE_BUILD_TYPE=Release . >/dev/null
ninja -C build/rp2040-zero >/dev/null
cp build/rp2040-zero/crsf_relay.uf2 "dist/crsf-relay-$VERSION-rp2040-zero.uf2"
echo "ok  dist/crsf-relay-$VERSION-rp2040-zero.uf2"
