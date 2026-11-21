#!/bin/bash
# Installs the iOS app on an iPhone or iPad, signed with the free personal team of the Apple ID signed in to Xcode. No
# AltStore and no other program sees the Apple ID: Xcode does the signing in.
#
#   ./ios/install-on-iphone.sh [--ipa FILE] [--device UDID] [--team TEAMID] [--icon FILE] [--auto]
#
# The app gets the icon of the Mac app installed on this Mac, or the image given with --icon (any format the system
# reads); the first one used is kept and used again by later runs. It stays on this Mac: nothing of it goes into the
# repository or a release.
#
# Without --ipa it installs pvz-portable-ios-arm64.ipa of the latest release of the GitHub repository of the remote
# "origin", so it also updates the app. A free team's signature lasts 7 days, that of a paid Apple Developer Program team
# a year; installing again renews it, the saves stay. --team picks the team when the keychain has certificates of
# several (a free and a paid one, say); the team used is kept and used again by later runs, --auto included. The app of
# each team has its own bundle identifier, so after a change of team it is a second app, with saves of its own.
# --auto is for ios/auto-renew.sh, which runs it every hour: it does nothing while the signature has more than 3 days
# left, and otherwise installs over Wi-Fi when it finds the iPhone, saying so in a notification only when it is urgent.
#
# Once beforehand: Xcode > Settings > Accounts, add the Apple ID; select its team ("(Personal Team)" for a free one),
# Manage Certificates, + > Apple Development. On the iPhone: Settings > Privacy & Security > Developer Mode on. The first run is over the
# cable, with the iPhone unlocked.
#
# How: a tiny app with the same bundle identifier is built with Xcode's automatic signing, which makes (or renews) the
# provisioning profile for this device; the game is then signed with that profile, the certificate in it and the
# entitlements Xcode gave the tiny app, and installed with devicectl.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WORK="$ROOT/build-ios-install"		# made anew by every run
STATE="$ROOT/build-ios-state"		# kept: the device, when the signature ends, the downloaded IPA, the log
RENEW_BEFORE=$((3 * 24 * 3600))		# --auto renews once fewer seconds than this are left
URGENT=$((24 * 3600))				# --auto notifies about a failure once fewer seconds than this are left
IPA=""
DEVICE=""
TEAM=""
AUTO=0
ICON=""
MAC_APP_ICON="/Applications/PlantsVsZombies.app/Contents/Resources/AppIcon.icns"

while [ $# -gt 0 ]; do
	case "$1" in
		--ipa) IPA="$2"; shift 2 ;;
		--icon) ICON="$2"; shift 2 ;;
		--device) DEVICE="$2"; shift 2 ;;
		--team) TEAM="$2"; shift 2 ;;
		--auto) AUTO=1; shift ;;
		-h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d'; exit 0 ;;
		*) echo "Unknown option $1 (see --help)" >&2; exit 2 ;;
	esac
done

mkdir -p "$STATE"
NOW="$(date +%s)"
EXPIRES_AT="$(cat "$STATE/expires" 2>/dev/null || echo 0)"
case "$EXPIRES_AT" in ''|*[!0-9]*) EXPIRES_AT=0 ;; esac

notify() {
	# only from --auto, and only while there is little time left: a run by hand shows its errors in the terminal
	[ "$AUTO" = 1 ] && [ $((EXPIRES_AT - NOW)) -lt "$URGENT" ] || return 0
	/usr/bin/osascript -e "display notification \"$1\" with title \"PvZ Portable\"" >/dev/null 2>&1 || true
}
fail() {
	echo "Error: $*" >&2
	notify "Could not renew the app on your iPhone: connect it with the cable and run ios/install-on-iphone.sh."
	exit 1
}

if [ "$AUTO" = 1 ]; then
	echo "--- $(date) ---"
	if [ $((EXPIRES_AT - NOW)) -gt "$RENEW_BEFORE" ]; then
		echo "Signature good until $(date -r "$EXPIRES_AT"), nothing to do"
		exit 0
	fi
	[ -n "$DEVICE" ] || DEVICE="$(cat "$STATE/device" 2>/dev/null || true)"
	[ -n "$DEVICE" ] || fail "no device yet: run ios/install-on-iphone.sh once by hand, over the cable"
fi

for tool in xcodebuild cmake security codesign python3 ditto curl; do
	command -v "$tool" >/dev/null 2>&1 || fail "$tool is missing (Xcode and Homebrew's cmake are needed)"
done
xcrun devicectl --version >/dev/null 2>&1 || fail "xcrun devicectl is missing: update Xcode (15 or later)"

rm -rf "$WORK"
mkdir -p "$WORK"

echo "=== iPhone ==="
xcrun devicectl list devices --json-output "$WORK/devices.json" >/dev/null 2>&1 || true
DEVICES="$(python3 -I - "$WORK/devices.json" <<'PY'
import json, sys
try:
    devices = json.load(open(sys.argv[1])).get("result", {}).get("devices", [])
except (OSError, ValueError):
    devices = []
for d in devices:
    hw = d.get("hardwareProperties", {})
    if hw.get("platform") == "iOS" and hw.get("udid"):
        state = d.get("connectionProperties", {}).get("tunnelState", "")
        print(hw["udid"], state, d.get("deviceProperties", {}).get("name", "?"), sep="\t")
PY
)"
if [ -z "$DEVICE" ]; then
	DEVICE="$(cat "$STATE/device" 2>/dev/null || true)"
	if [ -z "$DEVICE" ] || ! printf '%s\n' "$DEVICES" | cut -f1 | grep -qx "$DEVICE"; then
		COUNT="$(printf '%s' "$DEVICES" | grep -c . || true)"
		[ "$COUNT" = 1 ] || fail "$([ "$COUNT" = 0 ] && echo "no iPhone or iPad found: connect it with a cable, unlock it and tap Trust" \
			|| echo "several devices, pick one with --device UDID: $(printf '%s\n' "$DEVICES" | cut -f1,3 | tr '\t\n' ' ;')")"
		DEVICE="$(printf '%s\n' "$DEVICES" | cut -f1)"
	fi
fi
printf '%s\n' "$DEVICES" | cut -f1 | grep -qx "$DEVICE" || fail "the iPhone $DEVICE is not around (not on this Wi-Fi, or not paired)"
echo "Device $DEVICE"

[ -n "$TEAM" ] || TEAM="$(cat "$STATE/team" 2>/dev/null || true)"	# the team of the last install

echo "=== Signing certificate ==="
# Apple Development certificates whose private key is in the keychain, with the team (OU) of each
security find-identity -v -p codesigning > "$WORK/identities.txt"
security find-certificate -a -Z -p -c "Apple Development" > "$WORK/certificates.txt" 2>/dev/null || true
CERT_INFO="$(python3 -I - "$WORK/identities.txt" "$WORK/certificates.txt" "$TEAM" <<'PY'
import re, subprocess, sys
identities, certificates, wanted = sys.argv[1], sys.argv[2], sys.argv[3]
valid = set(re.findall(r'\b([0-9A-F]{40})\b\s+"Apple Development', open(identities).read()))
text = open(certificates).read()
found = []
for sha1, pem in re.findall(r'SHA-1 hash: ([0-9A-F]{40}).*?(-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----)', text, re.S):
    if sha1 not in valid:
        continue
    subject = subprocess.run(["openssl", "x509", "-noout", "-subject", "-nameopt", "RFC2253"], input=pem,
                             capture_output=True, text=True).stdout
    team = re.search(r'OU=([A-Z0-9]{10})', subject)
    if team and (not wanted or team.group(1) == wanted):
        found.append((sha1, team.group(1)))
teams = sorted({t for _, t in found})
if len(teams) != 1:
    print("NONE" if not teams else "MANY " + " ".join(teams))
else:
    print(found[0][0], teams[0])
PY
)"
case "$CERT_INFO" in
	NONE) fail "no Apple Development certificate in the keychain${TEAM:+ for team $TEAM}. In Xcode: Settings > Accounts > your Apple ID > (Personal Team) > Manage Certificates > + > Apple Development" ;;
	MANY*) fail "certificates of several teams (${CERT_INFO#MANY }): pick one with --team TEAMID" ;;
esac
TEAM="${CERT_INFO#* }"
echo "Team $TEAM"

# The identifier of this team's copy: an identifier belongs to one team, so the team ID makes it free to use
BUNDLE_ID="io.github.wszqkzqk.pvz-portable.$TEAM"
echo "Bundle identifier $BUNDLE_ID"

echo "=== Provisioning profile ==="
mkdir -p "$WORK/profile"
# never started: it is only built, so that Xcode signs it, and the game replaces it on the device
echo 'int main(void) { return 0; }' > "$WORK/profile/main.c"
cat > "$WORK/profile/CMakeLists.txt" <<CMAKE
cmake_minimum_required(VERSION 3.21)
project(PvzpSigning LANGUAGES C)
set(CMAKE_XCODE_GENERATE_SCHEME ON)
add_executable(PvzpSigning MACOSX_BUNDLE main.c)
set_target_properties(PvzpSigning PROPERTIES
	MACOSX_BUNDLE_GUI_IDENTIFIER "$BUNDLE_ID"
	XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "$BUNDLE_ID"
	XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "$TEAM"
	XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
	XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
	XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "16.4"
	XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
)
CMAKE
cmake -S "$WORK/profile" -B "$WORK/profile/build" -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
	-DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 > "$WORK/profile-cmake.log" 2>&1 || { tail -20 "$WORK/profile-cmake.log"; fail "could not make the signing project"; }
xcodebuild -project "$WORK/profile/build/PvzpSigning.xcodeproj" -scheme PvzpSigning -configuration Release \
	-destination "platform=iOS,id=$DEVICE" -allowProvisioningUpdates -allowProvisioningDeviceRegistration \
	build > "$WORK/profile-build.log" 2>&1 || {
		grep -E "error:|No Account|No profiles|not registered|Developer Mode" "$WORK/profile-build.log" | head -15
		fail "Xcode could not sign for this device (full log: $WORK/profile-build.log). Is the Apple ID added in Xcode > Settings > Accounts?"
	}
SIGNER_APP="$(find "$WORK/profile/build" -name "PvzpSigning.app" -path "*iphoneos*" -type d | head -n 1)"
[ -n "$SIGNER_APP" ] && [ -f "$SIGNER_APP/embedded.mobileprovision" ] || fail "Xcode made no provisioning profile (see $WORK/profile-build.log)"
cp "$SIGNER_APP/embedded.mobileprovision" "$WORK/embedded.mobileprovision"
codesign -d --entitlements - --xml "$SIGNER_APP" > "$WORK/entitlements.plist" 2>/dev/null
plutil -lint "$WORK/entitlements.plist" >/dev/null || fail "could not read the entitlements Xcode signed with"

# the certificate of the profile, which is the one to sign the game with, and when the profile ends
security cms -D -i "$WORK/embedded.mobileprovision" > "$WORK/profile.plist"
PROFILE_INFO="$(python3 -I - "$WORK/profile.plist" "$WORK/identities.txt" <<'PY'
import hashlib, plistlib, re, sys
profile = plistlib.load(open(sys.argv[1], "rb"))
valid = set(re.findall(r'\b([0-9A-F]{40})\b', open(sys.argv[2]).read()))
ends = int(profile["ExpirationDate"].timestamp()) if "ExpirationDate" in profile else 0
for cert in profile.get("DeveloperCertificates", []):
    sha1 = hashlib.sha1(cert).hexdigest().upper()
    if sha1 in valid:
        print(sha1, ends)
        break
PY
)"
[ -n "$PROFILE_INFO" ] || fail "the profile names no certificate whose private key is in this keychain"
SIGN_HASH="${PROFILE_INFO% *}"
NEW_EXPIRES="${PROFILE_INFO#* }"
echo "Profile valid until $(date -r "$NEW_EXPIRES")"

echo "=== Game ==="
if [ -z "$IPA" ]; then
	REPO="$(git -C "$ROOT" remote get-url origin 2>/dev/null \
		| sed -nE 's#^(https://([^@/]+@)?github\.com/|git@github\.com:)([^/]+/[^/]+)$#\3#p' | sed 's/\.git$//')"
	[ -n "$REPO" ] || fail "no GitHub remote origin to download the release from: pass --ipa FILE"
	IPA="$STATE/pvz-portable-ios-arm64.ipa"
	echo "Latest release of $REPO"
	# only downloaded again when the release has a newer one
	ZFLAG=()
	[ -f "$IPA" ] && ZFLAG=(-z "$IPA")
	curl -fsSL ${ZFLAG[@]+"${ZFLAG[@]}"} -o "$IPA.new" "https://github.com/$REPO/releases/latest/download/pvz-portable-ios-arm64.ipa" \
		|| { rm -f "$IPA.new"; [ -f "$IPA" ] || fail "could not download the IPA"; echo "Offline: using the IPA downloaded before"; }
	[ -s "$IPA.new" ] && mv "$IPA.new" "$IPA"
	rm -f "$IPA.new"
fi
[ -f "$IPA" ] || fail "no IPA at $IPA"

mkdir -p "$WORK/unpacked"
ditto -x -k "$IPA" "$WORK/unpacked"
APP="$(find "$WORK/unpacked/Payload" -maxdepth 1 -name "*.app" -type d | head -n 1)"
[ -n "$APP" ] || fail "the IPA holds no app"

/usr/libexec/PlistBuddy -c "Set :CFBundleIdentifier $BUNDLE_ID" "$APP/Info.plist"

# The icon: given with --icon, else the one kept from an earlier run, else that of the installed Mac app. It is drawn
# onto an opaque square (iOS shows transparent parts of an icon as black) and compiled into the app's asset catalog.
KEPT_ICON="$STATE/AppIcon-1024.png"
if [ -z "$ICON" ] && [ ! -f "$KEPT_ICON" ] && [ -f "$MAC_APP_ICON" ]; then
	ICON="$MAC_APP_ICON"
fi
if [ -n "$ICON" ]; then
	[ -f "$ICON" ] || fail "no icon at $ICON"
	echo "Icon from $ICON"
	cat > "$WORK/flatten-icon.swift" <<'SWIFT'
import AppKit
let arguments = CommandLine.arguments
guard arguments.count == 3, let image = NSImage(contentsOfFile: arguments[1]) else { exit(1) }
let side = 1024
guard let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: side, pixelsHigh: side, bitsPerSample: 8,
		samplesPerPixel: 4, hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0),
	let context = NSGraphicsContext(bitmapImageRep: bitmap) else { exit(1) }
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = context
let square = NSRect(x: 0, y: 0, width: side, height: side)
NSColor.white.setFill()
square.fill()
image.draw(in: square, from: .zero, operation: .sourceOver, fraction: 1)
NSGraphicsContext.restoreGraphicsState()
guard let png = bitmap.representation(using: .png, properties: [:]) else { exit(1) }
do { try png.write(to: URL(fileURLWithPath: arguments[2])) } catch { exit(1) }
SWIFT
	xcrun swift "$WORK/flatten-icon.swift" "$ICON" "$KEPT_ICON.new" > "$WORK/icon.log" 2>&1 \
		|| { tail -5 "$WORK/icon.log"; rm -f "$KEPT_ICON.new"; fail "could not read the icon $ICON"; }
	mv "$KEPT_ICON.new" "$KEPT_ICON"
fi
if [ -f "$KEPT_ICON" ]; then
	CATALOG="$WORK/icon/Assets.xcassets"
	mkdir -p "$CATALOG/AppIcon.appiconset"
	cp "$KEPT_ICON" "$CATALOG/AppIcon.appiconset/AppIcon-1024.png"
	echo '{"info":{"author":"xcode","version":1}}' > "$CATALOG/Contents.json"
	cat > "$CATALOG/AppIcon.appiconset/Contents.json" <<'JSON'
{"images":[{"filename":"AppIcon-1024.png","idiom":"universal","platform":"ios","size":"1024x1024"}],"info":{"author":"xcode","version":1}}
JSON
	xcrun actool "$CATALOG" --compile "$APP" --platform iphoneos --minimum-deployment-target 16.4 \
		--target-device iphone --target-device ipad --app-icon AppIcon \
		--output-partial-info-plist "$WORK/icon/partial.plist" > "$WORK/icon/actool.log" 2>&1 \
		|| { tail -10 "$WORK/icon/actool.log"; fail "could not make the app icon"; }
	# the icon entries actool wrote for the new catalog replace those of the release's
	python3 -I - "$APP/Info.plist" "$WORK/icon/partial.plist" <<'PY'
import plistlib, sys
info_path, partial_path = sys.argv[1], sys.argv[2]
with open(info_path, "rb") as f:
    info = plistlib.load(f)
with open(partial_path, "rb") as f:
    partial = plistlib.load(f)
for key in ("CFBundleIcons", "CFBundleIcons~ipad", "CFBundleIconName"):
    info.pop(key, None)
info.update(partial)
with open(info_path, "wb") as f:
    plistlib.dump(info, f, fmt=plistlib.FMT_BINARY)
PY
fi

cp "$WORK/embedded.mobileprovision" "$APP/embedded.mobileprovision"
rm -rf "$APP/_CodeSignature"
# whatever the app carries inside is signed first, the app last
while IFS= read -r -d '' anInner; do
	codesign -f -s "$SIGN_HASH" --timestamp=none "$anInner"
done < <(find "$APP" -mindepth 1 \( -name "*.framework" -o -name "*.dylib" -o -name "*.appex" \) -print0)
codesign -f -s "$SIGN_HASH" --timestamp=none --entitlements "$WORK/entitlements.plist" "$APP"
codesign --verify --deep --strict "$APP"
echo "Signed $(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP/Info.plist") (build $(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$APP/Info.plist"))"

echo "=== Installing ==="
xcrun devicectl device install app --device "$DEVICE" "$APP" > "$WORK/install.log" 2>&1 || {
	tail -15 "$WORK/install.log"
	fail "could not install on the iPhone: is it unlocked, and on the same Wi-Fi or on the cable?"
}
echo "$DEVICE" > "$STATE/device"
echo "$TEAM" > "$STATE/team"
echo "$NEW_EXPIRES" > "$STATE/expires"
echo "Installed; the signature lasts until $(date -r "$NEW_EXPIRES")"

if [ "$AUTO" = 0 ]; then
	cat <<EOF

The first time, open Plants vs. Zombies from the home screen; if iOS says the developer is not trusted:
Settings > General > VPN & Device Management > your Apple ID > Trust.
Then copy main.pak and properties/ into the Plants vs. Zombies folder of the Files app (Finder > your iPhone > Files).
To renew it by itself over Wi-Fi: ./ios/auto-renew.sh install
EOF
fi
