#!/bin/bash
# Renews the iOS app on the iPhone by itself, over Wi-Fi, before its free 7-day signature ends: a launchd job of this
# user runs "ios/install-on-iphone.sh --auto" every hour, which does nothing while more than 3 days are left.
#
#   ./ios/auto-renew.sh install     start the job (run ios/install-on-iphone.sh once by hand first)
#   ./ios/auto-renew.sh uninstall   stop it
#   ./ios/auto-renew.sh status      when the signature ends, and the end of the log
#
# A renewal needs this Mac awake, on the same Wi-Fi as the iPhone (with Xcode allowed on the local network), the
# iPhone reachable over Wi-Fi (Xcode > Window > Devices and Simulators > the iPhone > Connect via network) and on the
# internet. When it keeps failing with less than a day left, a notification says to use the cable.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STATE="$ROOT/build-ios-state"
LABEL="io.github.pvz-portable.ios-renew"
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
DOMAIN="gui/$(id -u)"

case "${1:-}" in
	install)
		[ -f "$STATE/device" ] || { echo "Run ./ios/install-on-iphone.sh once by hand first (over the cable)." >&2; exit 1; }
		mkdir -p "$(dirname "$PLIST")"
		# PATH: launchd starts jobs with a bare one, and the script needs Homebrew's cmake
		/usr/bin/python3 -I - "$PLIST" "$LABEL" "$ROOT/ios/install-on-iphone.sh" "$STATE/renew.log" <<'PY'
import plistlib, sys
path, label, script, log = sys.argv[1:5]
with open(path, "wb") as f:
    plistlib.dump({
        "Label": label,
        "ProgramArguments": ["/bin/bash", script, "--auto"],
        "StartInterval": 3600,
        "RunAtLoad": True,
        "StandardOutPath": log,
        "StandardErrorPath": log,
        "EnvironmentVariables": {"PATH": "/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin"},
        "ProcessType": "Background",
        "LowPriorityIO": True,
    }, f)
PY
		launchctl bootout "$DOMAIN/$LABEL" >/dev/null 2>&1 || true
		launchctl bootstrap "$DOMAIN" "$PLIST"
		echo "Auto-renewal is on: it checks every hour, and renews over Wi-Fi once fewer than 3 days are left."
		echo "Log: $STATE/renew.log"
		;;
	uninstall)
		launchctl bootout "$DOMAIN/$LABEL" >/dev/null 2>&1 || true
		rm -f "$PLIST"
		echo "Auto-renewal is off."
		;;
	status)
		[ -f "$PLIST" ] && echo "Auto-renewal: on" || echo "Auto-renewal: off"
		if [ -f "$STATE/expires" ]; then
			echo "Signature on the iPhone lasts until $(date -r "$(cat "$STATE/expires")")"
		fi
		[ -f "$STATE/renew.log" ] && { echo "--- last lines of $STATE/renew.log ---"; tail -15 "$STATE/renew.log"; }
		;;
	*)
		sed -n '2,12p' "$0"
		exit 2
		;;
esac
