#!/bin/bash
# Puts the icon made from icon.png into the installed app, /Applications/PlantsVsZombies.app, without building the
# game again: for when an update through Sparkle brought the icon of the release back.

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
APP="${1:-/Applications/PlantsVsZombies.app}"
[ -d "$APP/Contents/Resources" ] || { echo "No app at $APP" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
ICONSET="$WORK/AppIcon.iconset"
mkdir -p "$ICONSET"
for size in 16 32 128 256 512; do
	sips -z $size $size "$ROOT/icon.png" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
	sips -z $((size * 2)) $((size * 2)) "$ROOT/icon.png" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$WORK/AppIcon.icns"

cp "$WORK/AppIcon.icns" "$APP/Contents/Resources/AppIcon.icns"
# the bundle's ad-hoc signature covers its resources: sign it again over the new icon
codesign --force -s - "$APP"
codesign --verify --deep --strict "$APP"
# Finder and the Dock cache icons: tell them the app changed
touch "$APP"
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$APP"
killall Dock 2>/dev/null || true
echo "Icon of $APP updated from icon.png"
