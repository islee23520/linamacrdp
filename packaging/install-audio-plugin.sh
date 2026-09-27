#!/bin/bash
# Explicit manual install only. Build first with make-audio-plugin.sh.
# Usage: sudo packaging/install-audio-plugin.sh
set -euo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
source="$root/target/macrdp-microphone.driver"
target="/Library/Audio/Plug-Ins/HAL/macrdp-microphone.driver"
if [[ $EUID -ne 0 ]]; then echo 'Run with sudo to install into the system HAL directory.' >&2; exit 1; fi
if [[ ! -f "$source/Contents/MacOS/macrdpmic" ]]; then
    echo 'Run packaging/make-audio-plugin.sh first.' >&2; exit 1
fi
if [[ -e "$target" ]]; then echo "Refusing to replace $target" >&2; exit 1; fi
mkdir -p "$(dirname "$target")"
ditto "$source" "$target"
chown -R root:wheel "$target"
chmod -R go-w "$target"
echo "Installed $target. Restart coreaudiod or reboot to load it."
