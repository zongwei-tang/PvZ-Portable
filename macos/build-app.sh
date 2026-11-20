#!/bin/bash
# Build PvZ-Portable as a Mac app and install it to /Applications/PlantsVsZombies.app.
#
# Usage: ./macos/build-app.sh [--data DIR] [--no-data] [--no-install]
#   --data DIR    folder with main.pak and properties/, copied into the per-user data folder
#                 (default: what is there already, else taken from the installed app)
#   --no-data     leave the game data alone (for release builds, which ship without it)
#   --no-install  only build build-mac/PlantsVsZombies.app
#
# The app carries no game data, so that an update can replace it: main.pak and properties/ live in
# ~/Library/Application Support/io.github.wszqkzqk/PvZPortable/, next to the saves. The app updates itself through
# Sparkle from the releases of the GitHub repository of the remote "origin" (see .github/workflows/mac-release.yml).
#
# Needs Homebrew; missing build tools and libraries are installed with it.
# OAuth clients to build in (so players sign in with the button alone) are read from macos/cloud-clients.cmake, which is
# not in git, or from CLOUD_SAVE_GOOGLE_CLIENT_ID, CLOUD_SAVE_GOOGLE_CLIENT_SECRET and CLOUD_SAVE_ONEDRIVE_CLIENT_ID.
# APP_VERSION sets the version the app shows (default: from the latest tag); GITHUB_REPOSITORY (owner/name) the
# repository whose releases it updates from.

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-mac"
APP_NAME="PlantsVsZombies.app"
INSTALLED="/Applications/$APP_NAME"
SUPPORT="$HOME/Library/Application Support/io.github.wszqkzqk/PvZPortable"
DATA=""
USE_DATA=1
INSTALL=1

SPARKLE_VERSION="2.10.1"
SPARKLE_SHA256="1a856d2a9edb8e628de88121d7be04cf0e2167f8641650b78fca00b14244cf27"

while [ $# -gt 0 ]; do
	case "$1" in
		--data) DATA="${2:?--data needs a folder}"; shift 2 ;;
		--no-data) USE_DATA=0; shift ;;
		--no-install) INSTALL=0; shift ;;
		-h|--help) sed -n '2,19p' "$0"; exit 0 ;;
		*) echo "Unknown option: $1" >&2; exit 1 ;;
	esac
done

step() { echo; echo "=== $* ==="; }
has_data() { [ -f "$1/main.pak" ] && [ -d "$1/properties" ]; }

# --- game data: main.pak and properties/ are not in git ---
if [ "$USE_DATA" = 1 ]; then
	step "Game data"
	if [ -z "$DATA" ]; then
		if has_data "$SUPPORT"; then
			echo "Found in $SUPPORT"
		else
			# apps made by earlier versions of this script carry the data in Contents/Resources or Contents/MacOS
			for dir in "$INSTALLED/Contents/Resources" "$INSTALLED/Contents/MacOS"; do
				if has_data "$dir"; then DATA="$dir"; break; fi
			done
			if [ -z "$DATA" ]; then
				echo "No game data found in $SUPPORT or $INSTALLED." >&2
				echo "Pass the folder that holds main.pak and properties/ with --data DIR." >&2
				exit 1
			fi
		fi
	fi
	if [ -n "$DATA" ]; then
		has_data "$DATA" || { echo "$DATA needs main.pak and a properties/ folder" >&2; exit 1; }
		if [ "$(cd "$DATA" && pwd -P)" != "$(mkdir -p "$SUPPORT" && cd "$SUPPORT" && pwd -P)" ]; then
			echo "Copying main.pak and properties/ from $DATA to $SUPPORT"
			cp "$DATA/main.pak" "$SUPPORT/main.pak.new"
			rm -rf "$SUPPORT/properties.new"
			cp -R "$DATA/properties" "$SUPPORT/properties.new"
			mv -f "$SUPPORT/main.pak.new" "$SUPPORT/main.pak"
			rm -rf "$SUPPORT/properties"
			mv "$SUPPORT/properties.new" "$SUPPORT/properties"
		fi
	fi
fi

# --- dependencies ---
step "Checking Homebrew packages"
command -v brew >/dev/null || { echo "Homebrew is needed: https://brew.sh" >&2; exit 1; }
BREW_PREFIX="$(brew --prefix 2>/dev/null || echo /opt/homebrew)"
missing=()
for pkg in cmake ninja dylibbundler jpeg-turbo libopenmpt libpng sdl3 sdl3_mixer; do
	[ -d "$BREW_PREFIX/opt/$pkg" ] || missing+=("$pkg")
done
if [ ${#missing[@]} -gt 0 ]; then
	echo "Installing: ${missing[*]}"
	brew install "${missing[@]}"
else
	echo "All installed."
fi

# --- Sparkle, the updater the app carries ---
step "Sparkle $SPARKLE_VERSION"
SPARKLE="$BUILD/sparkle-$SPARKLE_VERSION"
if [ ! -d "$SPARKLE/Sparkle.framework" ]; then
	mkdir -p "$BUILD"
	ARCHIVE="$BUILD/Sparkle-$SPARKLE_VERSION.tar.xz"
	curl -fL --retry 3 -o "$ARCHIVE" \
		"https://github.com/sparkle-project/Sparkle/releases/download/$SPARKLE_VERSION/Sparkle-$SPARKLE_VERSION.tar.xz"
	echo "$SPARKLE_SHA256  $ARCHIVE" | shasum -a 256 -c - >/dev/null \
		|| { echo "Sparkle-$SPARKLE_VERSION.tar.xz does not match its checksum" >&2; rm -f "$ARCHIVE"; exit 1; }
	rm -rf "$SPARKLE.part"
	mkdir -p "$SPARKLE.part"
	tar -xJf "$ARCHIVE" -C "$SPARKLE.part"
	rm -f "$ARCHIVE"
	mv "$SPARKLE.part" "$SPARKLE"
fi
echo "In $SPARKLE"

# where the app looks for updates: the latest release of the GitHub repository
REPO="${GITHUB_REPOSITORY:-}"
if [ -z "$REPO" ]; then
	REPO="$(git -C "$ROOT" remote get-url origin 2>/dev/null \
		| sed -nE 's#^(https://([^@/]+@)?github\.com/|git@github\.com:)([^/]+/[^/]+)$#\3#p' | sed 's/\.git$//')"
fi
PUBLIC_KEY="$(tr -d ' \n' < "$ROOT/macos/sparkle-public-key.txt" 2>/dev/null || true)"
if [ -n "$REPO" ] && [ -n "$PUBLIC_KEY" ]; then
	FEED_URL="https://github.com/$REPO/releases/latest/download/appcast.xml"
	echo "Updates from $FEED_URL"
else
	FEED_URL=""
	echo "Note: no GitHub repository or no macos/sparkle-public-key.txt, so this app does not update itself."
fi

# --- build ---
step "Building"
# --fresh: a cache from an earlier build keeps its old defaults (such as the built-in clients) and the SDL it found
cmake_args=(--fresh -G Ninja -B "$BUILD/game" -S "$ROOT" -DCMAKE_BUILD_TYPE=Release)
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
mkdir -p "$STAGE/Contents/MacOS" "$STAGE/Contents/Resources" "$STAGE/Contents/Frameworks"
MACOS="$STAGE/Contents/MacOS"

cp "$BUILD/game/pvz-portable" "$MACOS/pvz-portable"
strip -x "$MACOS/pvz-portable"
cp "$ROOT/macos/launcher.sh" "$MACOS/PlantsVsZombies"
chmod +x "$MACOS/PlantsVsZombies"

# the libraries go into the bundle, so the app runs without Homebrew
dylibbundler -of -cd -b -x "$MACOS/pvz-portable" -d "$MACOS/libs" -p @executable_path/libs/ >"$BUILD/dylibbundler.log" 2>&1 || { cat "$BUILD/dylibbundler.log" >&2; exit 1; }
# Sparkle keeps the signature of its makers; the game loads it at run time from here
ditto "$SPARKLE/Sparkle.framework" "$STAGE/Contents/Frameworks/Sparkle.framework"

# icon: made from icon.png, the icon of every version of the game, so a local build looks like the release
ICONSET="$BUILD/AppIcon.iconset"
rm -rf "$ICONSET"
mkdir -p "$ICONSET"
for size in 16 32 128 256 512; do
	sips -z $size $size "$ROOT/icon.png" --out "$ICONSET/icon_${size}x${size}.png" >/dev/null
	sips -z $((size * 2)) $((size * 2)) "$ROOT/icon.png" --out "$ICONSET/icon_${size}x${size}@2x.png" >/dev/null
done
iconutil -c icns "$ICONSET" -o "$STAGE/Contents/Resources/AppIcon.icns"

# Sparkle offers an update when the release has a higher CFBundleVersion: the number of commits only goes up
BUILD_NUMBER="$(git -C "$ROOT" rev-list --count HEAD)"
VERSION="${APP_VERSION:-$(git -C "$ROOT" describe --tags --abbrev=0 2>/dev/null || true)}"
VERSION="$(printf '%s' "$VERSION" | sed 's/^v//' | grep -oE '^[0-9]+(\.[0-9]+)*' || true)"
VERSION="${VERSION:-0.1}"
UPDATER_KEYS=""
if [ -n "$FEED_URL" ]; then
	UPDATER_KEYS="	<key>SUFeedURL</key>
	<string>$FEED_URL</string>
	<key>SUPublicEDKey</key>
	<string>$PUBLIC_KEY</string>
	<key>SUEnableAutomaticChecks</key>
	<true/>"
fi
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
	<string>$BUILD_NUMBER</string>
	<key>LSMinimumSystemVersion</key>
	<string>12.0</string>
	<key>NSHighResolutionCapable</key>
	<true/>
$UPDATER_KEYS
</dict>
</plist>
EOF
plutil -lint "$STAGE/Contents/Info.plist" >/dev/null

# ad-hoc signatures: Apple Silicon runs no unsigned code. The bundle is signed last, over what it holds.
for lib in "$MACOS"/libs/*.dylib; do
	codesign --force -s - "$lib" 2>/dev/null
done
codesign --force -s - "$MACOS/pvz-portable" 2>/dev/null

rm -rf "$APP"
mv "$STAGE" "$APP"
codesign --force -s - "$APP"
codesign --verify --deep --strict "$APP"
echo "Built $APP (version $VERSION, build $BUILD_NUMBER)"

# --- install ---
if [ "$INSTALL" = 1 ]; then
	step "Installing to $INSTALLED"
	if pgrep -x pvz-portable >/dev/null; then
		echo "The game is running. Quit it (Cmd+Q) and run this again." >&2
		exit 1
	fi
	rm -rf "$INSTALLED"
	ditto "$APP" "$INSTALLED"
	echo "Installed. Your saves and the game data are in $SUPPORT, so they stay."
fi
