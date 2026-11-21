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
# a year; installing again renews it, the saves stay. When the keychain has certificates of several teams (a free and a
# paid one, say) it uses the one whose signature lasts longest, so the paid one, unless --team picks another; the team
# used is kept for later runs, --auto included. The app of each team has its own bundle identifier, so after a change
# of team it is a second app, with saves of its own.
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

echo "=== Signing certificate ==="
# The teams that have an Apple Development certificate whose private key is in the keychain (the team is its OU)
security find-identity -v -p codesigning > "$WORK/identities.txt"
security find-certificate -a -Z -p -c "Apple Development" > "$WORK/certificates.txt" 2>/dev/null || true
TEAMS="$(python3 -I - "$WORK/identities.txt" "$WORK/certificates.txt" <<'PY'
import re, subprocess, sys
identities, certificates = sys.argv[1], sys.argv[2]
valid = set(re.findall(r'\b([0-9A-F]{40})\b\s+"Apple Development', open(identities).read()))
text = open(certificates).read()
teams = set()
for sha1, pem in re.findall(r'SHA-1 hash: ([0-9A-F]{40}).*?(-----BEGIN CERTIFICATE-----.*?-----END CERTIFICATE-----)', text, re.S):
    if sha1 not in valid:
        continue
    subject = subprocess.run(["openssl", "x509", "-noout", "-subject", "-nameopt", "RFC2253"], input=pem,
                             capture_output=True, text=True).stdout
    team = re.search(r'OU=([A-Z0-9]{10})', subject)
    if team:
        teams.add(team.group(1))
print(" ".join(sorted(teams)))
PY
)"
case " $TEAMS " in *" ${TEAM:-$TEAMS} "*) ;; *) TEAMS="" ;; esac
[ -n "$TEAMS" ] || fail "no Apple Development certificate in the keychain${TEAM:+ for team $TEAM}. In Xcode: Settings > Accounts > your Apple ID > its team > Manage Certificates > + > Apple Development"

# A provisioning profile of one team for this device, made by Xcode's automatic signing of a tiny app with the same
# bundle identifier, in $WORK/profile-TEAM. Prints the certificate to sign with and when the profile ends.
make_profile() {
	local aTeam="$1" aDir="$WORK/profile-$1" aBundle="io.github.wszqkzqk.pvz-portable.$1"
	mkdir -p "$aDir/src"
	# never started: it is only built, so that Xcode signs it, and the game replaces it on the device
	echo 'int main(void) { return 0; }' > "$aDir/src/main.c"
	cat > "$aDir/src/CMakeLists.txt" <<CMAKE
cmake_minimum_required(VERSION 3.21)
project(PvzpSigning LANGUAGES C)
set(CMAKE_XCODE_GENERATE_SCHEME ON)
add_executable(PvzpSigning MACOSX_BUNDLE main.c)
set_target_properties(PvzpSigning PROPERTIES
	MACOSX_BUNDLE_GUI_IDENTIFIER "$aBundle"
	XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "$aBundle"
	XCODE_ATTRIBUTE_DEVELOPMENT_TEAM "$aTeam"
	XCODE_ATTRIBUTE_CODE_SIGN_STYLE "Automatic"
	XCODE_ATTRIBUTE_CODE_SIGN_IDENTITY "Apple Development"
	XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "16.4"
	XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
)
CMAKE
	cmake -S "$aDir/src" -B "$aDir/build" -G Xcode -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_ARCHITECTURES=arm64 \
		-DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 > "$aDir/cmake.log" 2>&1 \
		|| { tail -20 "$aDir/cmake.log" >&2; echo "Team $aTeam: could not make the signing project" >&2; return 1; }
	xcodebuild -project "$aDir/build/PvzpSigning.xcodeproj" -scheme PvzpSigning -configuration Release \
		-destination "platform=iOS,id=$DEVICE" -allowProvisioningUpdates -allowProvisioningDeviceRegistration \
		build > "$aDir/build.log" 2>&1 || {
			grep -E "error:|No Account|No profiles|not registered|Developer Mode" "$aDir/build.log" | head -15 >&2
			echo "Team $aTeam: Xcode could not sign for this device (full log: $aDir/build.log). Is its Apple ID added in Xcode > Settings > Accounts?" >&2
			return 1
		}
	local aSigner
	aSigner="$(find "$aDir/build" -name "PvzpSigning.app" -path "*iphoneos*" -type d | head -n 1)"
	[ -n "$aSigner" ] && [ -f "$aSigner/embedded.mobileprovision" ] \
		|| { echo "Team $aTeam: Xcode made no provisioning profile (see $aDir/build.log)" >&2; return 1; }
	cp "$aSigner/embedded.mobileprovision" "$aDir/embedded.mobileprovision"
	codesign -d --entitlements - --xml "$aSigner" > "$aDir/entitlements.plist" 2>/dev/null
	plutil -lint "$aDir/entitlements.plist" >/dev/null \
		|| { echo "Team $aTeam: could not read the entitlements Xcode signed with" >&2; return 1; }
	# the certificate of the profile, which is the one to sign the game with, and when the profile ends
	security cms -D -i "$aDir/embedded.mobileprovision" > "$aDir/profile.plist"
	local anInfo
	anInfo="$(python3 -I - "$aDir/profile.plist" "$WORK/identities.txt" <<'PY'
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
	[ -n "$anInfo" ] || { echo "Team $aTeam: the profile names no certificate whose private key is in this keychain" >&2; return 1; }
	echo "$anInfo"
}

echo "=== Team ==="
# Given with --team, or the only one; with several (a free and a paid one, say) each gets a profile and the one whose
# profile lasts longest is used: a paid team's lasts a year, a free one's 7 days. That team is kept, and used again
# as long as the same teams have certificates.
KEPT_TEAM="$(cat "$STATE/team" 2>/dev/null || true)"
KEPT_TEAMS="$(cat "$STATE/teams" 2>/dev/null || true)"
if [ -n "$TEAM" ] || [ "${TEAMS// /}" = "$TEAMS" ]; then
	CANDIDATES="${TEAM:-$TEAMS}"
elif [ -n "$KEPT_TEAM" ] && [ "$KEPT_TEAMS" = "$TEAMS" ]; then
	CANDIDATES="$KEPT_TEAM"
else
	CANDIDATES="$TEAMS"
	echo "Certificates of several teams ($TEAMS): the one whose signature lasts longest is used"
fi
TEAM=""
NEW_EXPIRES=0
for aCandidate in $CANDIDATES; do
	echo "Profile of team $aCandidate"
	anInfo="$(make_profile "$aCandidate")" || continue
	anEnd="${anInfo#* }"
	echo "  valid until $(date -r "$anEnd")"
	if [ "$anEnd" -gt "$NEW_EXPIRES" ]; then
		TEAM="$aCandidate"
		SIGN_HASH="${anInfo% *}"
		NEW_EXPIRES="$anEnd"
	fi
done
[ -n "$TEAM" ] || fail "no team could sign for this device (see the messages above)"
cp "$WORK/profile-$TEAM/embedded.mobileprovision" "$WORK/embedded.mobileprovision"
cp "$WORK/profile-$TEAM/entitlements.plist" "$WORK/entitlements.plist"
# The identifier of this team's copy: an identifier belongs to one team, so the team ID makes it free to use
BUNDLE_ID="io.github.wszqkzqk.pvz-portable.$TEAM"
echo "Team $TEAM, bundle identifier $BUNDLE_ID"
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
echo "$TEAMS" > "$STATE/teams"
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
