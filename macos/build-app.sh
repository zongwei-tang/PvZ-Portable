#!/bin/bash
# Build PvZ-Portable as a Mac app and install it to /Applications/PlantsVsZombies.app.
#
# Usage: ./macos/build-app.sh [--data DIR] [--no-install]
#   --data DIR    folder with main.pak and properties/ (default: taken from the installed app)
#   --no-install  only build build-mac/PlantsVsZombies.app
#
# Needs Homebrew; missing build tools and libraries are installed with it.
# OAuth clients to build in (so players sign in with the button alone) are read from macos/cloud-clients.cmake, which is
# not in git, or from CLOUD_SAVE_GOOGLE_CLIENT_ID, CLOUD_SAVE_GOOGLE_CLIENT_SECRET and CLOUD_SAVE_ONEDRIVE_CLIENT_ID.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-mac"
APP_NAME="PlantsVsZombies.app"
INSTALLED="/Applications/$APP_NAME"
DATA=""
INSTALL=1

while [ $# -gt 0 ]; do
	case "$1" in
		--data) DATA="${2:?--data needs a folder}"; shift 2 ;;
		--no-install) INSTALL=0; shift ;;
		-h|--help) sed -n '2,10p' "$0"; exit 0 ;;
		*) echo "Unknown option: $1" >&2; exit 1 ;;
	esac
done

step() { echo; echo "=== $* ==="; }

# --- game data: main.pak and properties/ are not in git ---
if [ -z "$DATA" ]; then
	DATA="$INSTALLED/Contents/MacOS"
	if [ ! -f "$DATA/main.pak" ]; then
		echo "No game data found: $INSTALLED has no main.pak." >&2
		echo "Pass the folder that holds main.pak and properties/ with --data DIR." >&2
		exit 1
	fi
fi
[ -f "$DATA/main.pak" ] || { echo "$DATA has no main.pak" >&2; exit 1; }
[ -d "$DATA/properties" ] || { echo "$DATA has no properties/ folder" >&2; exit 1; }

# --- dependencies ---
step "Checking Homebrew packages"
command -v brew >/dev/null || { echo "Homebrew is needed: https://brew.sh" >&2; exit 1; }
BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
missing=()
for pkg in cmake ninja dylibbundler jpeg-turbo libopenmpt libpng sdl2; do
	[ -d "$BREW_PREFIX/opt/$pkg" ] || missing+=("$pkg")
done
if [ ${#missing[@]} -gt 0 ]; then
	echo "Installing: ${missing[*]}"
	brew install "${missing[@]}"
else
	echo "All installed."
fi

# --- build ---
step "Building"
cmake_args=(-G Ninja -B "$BUILD/game" -S "$ROOT" -DCMAKE_BUILD_TYPE=Release)
if [ -f "$ROOT/macos/cloud-clients.cmake" ]; then
	echo "Building in the OAuth clients of macos/cloud-clients.cmake"
	cmake_args+=(-C "$ROOT/macos/cloud-clients.cmake")
fi
for var in CLOUD_SAVE_GOOGLE_CLIENT_ID CLOUD_SAVE_GOOGLE_CLIENT_SECRET CLOUD_SAVE_ONEDRIVE_CLIENT_ID; do
	if [ -n "${!var:-}" ]; then cmake_args+=("-D$var=${!var}"); fi
done
cmake "${cmake_args[@]}"
cmake --build "$BUILD/game"
if ! grep -q '^CLOUD_SAVE_GOOGLE_CLIENT_ID:STRING=.' "$BUILD/game/CMakeCache.txt"; then
	echo "Note: no Google client is built in, so Google Drive asks players for a client ID (see macos/cloud-clients.cmake in the README)."
fi

# --- bundle ---
step "Making $APP_NAME"
APP="$BUILD/$APP_NAME"
STAGE="$BUILD/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/Contents/MacOS" "$STAGE/Contents/Resources"
MACOS="$STAGE/Contents/MacOS"

cp "$BUILD/game/pvz-portable" "$MACOS/pvz-portable"
strip -x "$MACOS/pvz-portable"
cp "$ROOT/macos/launcher.sh" "$MACOS/PlantsVsZombies"
chmod +x "$MACOS/PlantsVsZombies"
cp "$DATA/main.pak" "$MACOS/main.pak"
cp -R "$DATA/properties" "$MACOS/properties"

# the libraries go into the bundle, so the app runs without Homebrew
dylibbundler -of -cd -b -x "$MACOS/pvz-portable" -d "$MACOS/libs" -p @executable_path/libs/ >"$BUILD/dylibbundler.log" 2>&1 || { cat "$BUILD/dylibbundler.log" >&2; exit 1; }
# Homebrew's sdl2 is sdl2-compat, which loads SDL3 at run time, so dylibbundler cannot see it
if [ -f "$BREW_PREFIX/opt/sdl3/lib/libSDL3.dylib" ]; then
	cp -L "$BREW_PREFIX/opt/sdl3/lib/libSDL3.dylib" "$MACOS/libs/libSDL3.dylib"
	chmod u+w "$MACOS/libs/libSDL3.dylib"
fi

# icon: the one of the installed app, else made from icon.png
if [ -f "$INSTALLED/Contents/Resources/AppIcon.icns" ]; then
	cp "$INSTALLED/Contents/Resources/AppIcon.icns" "$STAGE/Contents/Resources/AppIcon.icns"
else
	ICONSET="$BUILD/AppIcon.iconset"
	rm -rf "$ICONSET"
	mkdir -p "$ICONSET"
	for size in 16 32 128 256 512; do
		sips -z $size $size "$ROOT/icon.png" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
		sips -z $((size * 2)) $((size * 2)) "$ROOT/icon.png" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
	done
	iconutil -c icns "$ICONSET" -o "$STAGE/Contents/Resources/AppIcon.icns"
fi

VERSION="$(git -C "$ROOT" describe --tags 2>/dev/null | sed 's/^v//' | grep -oE '^[0-9]+(\.[0-9]+)*' || true)"
VERSION="${VERSION:-0.1}"
cat > "$STAGE/Contents/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDisplayName</key>
	<string>Plants vs. Zombies</string>
	<key>CFBundleExecutable</key>
	<string>PlantsVsZombies</string>
	<key>CFBundleIconFile</key>
	<string>AppIcon</string>
	<key>CFBundleIdentifier</key>
	<string>io.github.wszqkzqk.pvz-portable</string>
	<key>CFBundleName</key>
	<string>Plants vs. Zombies</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleShortVersionString</key>
	<string>$VERSION</string>
	<key>CFBundleVersion</key>
	<string>$VERSION</string>
	<key>LSMinimumSystemVersion</key>
	<string>11.0</string>
	<key>NSHighResolutionCapable</key>
	<true/>
</dict>
</plist>
EOF

# ad-hoc signatures: Apple Silicon runs no unsigned code
for lib in "$MACOS"/libs/*.dylib; do
	codesign --force -s - "$lib" 2>/dev/null
done
codesign --force -s - "$MACOS/pvz-portable" 2>/dev/null

rm -rf "$APP"
mv "$STAGE" "$APP"
echo "Built $APP"

# --- install ---
if [ "$INSTALL" = 1 ]; then
	step "Installing to $INSTALLED"
	if pgrep -x pvz-portable >/dev/null; then
		echo "The game is running. Quit it (Cmd+Q) and run this again." >&2
		exit 1
	fi
	rm -rf "$INSTALLED"
	ditto "$APP" "$INSTALLED"
	echo "Installed. Your saves are in the per-user data folder, so they stay."
fi
