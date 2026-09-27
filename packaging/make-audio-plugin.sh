#!/bin/bash
# Compile the HAL input driver into target/macrdp-microphone.driver.
# Producer: 127.0.0.1:49228 TCP, raw S16LE mono PCM at 48000 Hz.
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
bundle="$root/target/macrdp-microphone.driver"
mkdir -p "$bundle/Contents/MacOS"
cp "$root/packaging/audio-plugin-Info.plist" "$bundle/Contents/Info.plist"
clang -std=c11 -O2 -Wall -Wextra -Werror -fPIC -dynamiclib \
    -framework CoreAudio -framework CoreFoundation \
    "$root/gui/Sources/macrdpmic/microphone.c" -o "$bundle/Contents/MacOS/macrdpmic"
plutil -lint "$bundle/Contents/Info.plist"
codesign -s - --force --deep "$bundle"
codesign --verify --deep --strict --verbose=2 "$bundle"
echo "$bundle"
