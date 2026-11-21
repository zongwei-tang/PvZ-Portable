#!/bin/bash
# Copies the game data of one copy of the iOS app into another on the same iPhone: main.pak, properties/ and the saves,
# all of which the game keeps in its Documents folder. For after the app was installed with another team (a second app,
# with a bundle identifier of its own) or from AltStore: the new copy starts empty.
#
#   ./ios/copy-data.sh [--from BUNDLE_ID] [--to BUNDLE_ID] [--device UDID]
#
# --to defaults to the copy that ios/install-on-iphone.sh installed last; --from to the other PvZ Portable on the
# device. Files of the same name in the new copy are replaced. Close the game on the iPhone before, and keep it unlocked.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
STATE="$ROOT/build-ios-state"
FROM=""
TO=""
DEVICE=""
while [ $# -gt 0 ]; do
	case "$1" in
		--from) FROM="$2"; shift 2 ;;
		--to) TO="$2"; shift 2 ;;
		--device) DEVICE="$2"; shift 2 ;;
		-h|--help) sed -n '2,/^set -euo/p' "$0" | sed '$d'; exit 0 ;;
		*) echo "Unknown option $1 (see --help)" >&2; exit 2 ;;
	esac
done
fail() { echo "Error: $*" >&2; exit 1; }

[ -n "$DEVICE" ] || DEVICE="$(cat "$STATE/device" 2>/dev/null || true)"
[ -n "$DEVICE" ] || fail "no device: pass --device UDID, or install with ios/install-on-iphone.sh first"
if [ -z "$TO" ]; then
	aTeam="$(cat "$STATE/team" 2>/dev/null || true)"
	[ -n "$aTeam" ] || fail "no copy installed by ios/install-on-iphone.sh yet: pass --to BUNDLE_ID"
	TO="io.github.wszqkzqk.pvz-portable.$aTeam"
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "=== Copies of the game on $DEVICE ==="
xcrun devicectl device info apps --device "$DEVICE" --json-output "$WORK/apps.json" >/dev/null 2>"$WORK/apps.log" \
	|| { tail -5 "$WORK/apps.log"; fail "could not list the apps: is the iPhone unlocked, and on the cable or this Wi-Fi?"; }
APPS="$(python3 -I - "$WORK/apps.json" <<'PY'
import json, sys
found = []
def walk(node):
    if isinstance(node, dict):
        bundle = node.get("bundleIdentifier")
        if isinstance(bundle, str) and "pvz-portable" in bundle.lower():
            found.append((bundle, node.get("name", "?")))
        for value in node.values():
            walk(value)
    elif isinstance(node, list):
        for value in node:
            walk(value)
walk(json.load(open(sys.argv[1])))
for bundle, name in sorted(set(found)):
    print(bundle, name, sep="\t")
PY
)"
[ -n "$APPS" ] || fail "no PvZ Portable on the device"
printf '%s\n' "$APPS" | sed 's/^/  /'
printf '%s\n' "$APPS" | cut -f1 | grep -qx "$TO" || fail "$TO is not on the device (pick the new copy with --to)"
if [ -z "$FROM" ]; then
	OTHERS="$(printf '%s\n' "$APPS" | cut -f1 | grep -vx "$TO" || true)"
	COUNT="$(printf '%s' "$OTHERS" | grep -c . || true)"
	[ "$COUNT" = 1 ] || fail "$([ "$COUNT" = 0 ] && echo "no other copy to copy from" \
		|| echo "several other copies, pick one with --from: $(echo $OTHERS)")"
	FROM="$OTHERS"
fi
[ "$FROM" != "$TO" ] || fail "--from and --to are the same app"
echo "From $FROM"
echo "To   $TO"

echo "=== Fetching the data of $FROM ==="
xcrun devicectl device copy from --device "$DEVICE" --domain-type appDataContainer --domain-identifier "$FROM" \
	--source Documents --destination "$WORK/from" >"$WORK/from.log" 2>&1 \
	|| { tail -10 "$WORK/from.log"; fail "could not read the Documents of $FROM"; }
# the folder that holds main.pak, wherever the copy put it
PAK="$(find "$WORK/from" -maxdepth 3 -name main.pak -type f | head -n 1)"
[ -n "$PAK" ] || fail "$FROM has no main.pak in its Documents: nothing to copy"
DATA="$(dirname "$PAK")"
echo "Found: $(cd "$DATA" && ls -1A | tr '\n' ' ')"

echo "=== Putting it into $TO ==="
for anItem in "$DATA"/* "$DATA"/.[!.]*; do
	[ -e "$anItem" ] || continue
	aName="$(basename "$anItem")"
	echo "  $aName"
	xcrun devicectl device copy to --device "$DEVICE" --domain-type appDataContainer --domain-identifier "$TO" \
		--source "$anItem" --destination "Documents/$aName" >"$WORK/to.log" 2>&1 \
		|| { tail -10 "$WORK/to.log"; fail "could not write $aName into $TO"; }
done

echo "=== Checking ==="
xcrun devicectl device info files --device "$DEVICE" --domain-type appDataContainer --domain-identifier "$TO" \
	--subdirectory Documents --no-recurse > "$WORK/check.txt" 2>&1 || true
grep -q "main.pak" "$WORK/check.txt" || { cat "$WORK/check.txt"; fail "main.pak is not in the Documents of $TO after the copy"; }
grep -q "properties" "$WORK/check.txt" || echo "Warning: no properties folder in the Documents of $TO" >&2
echo "Done: open the game on the iPhone. Once it runs well, the old copy ($FROM) can be deleted."
