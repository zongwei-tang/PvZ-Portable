#!/bin/bash
# The phone layout tested in the running game, on a Mac: the window takes the size of a phone in landscape, and the game
# plays some levels by itself (src/Lawn/System/PhoneLayoutSelfTest.h). Prints a report, also kept in
# build-phone/selftest-report.txt, and exits 1 when a check failed.
#
#   tests/phone_layout_selftest.sh build    build build-phone/pvz-portable with the self-test in it
#   tests/phone_layout_selftest.sh run [phone...] [level...]
#                                           run it on each phone (opens a window for a minute or two each)
#   tests/phone_layout_selftest.sh show [phone] [level]
#                                           open the game in the window of a phone, in a level (survival-day by default),
#                                           to play it yourself, without sound; saves in build-phone/show-save/
#
# Phones: iphone15, iphonese, iphone15promax (all by default). Levels: adventure-1-1, adventure-1-3, survival-day,
# survival-day-10-packets, survival-night, survival-pool, survival-fog, survival-roof (all by default).
#
# The game needs main.pak and properties/ in build-phone/ (git ignores the folder). Its saves go to
# build-phone/selftest-save/, made anew for each run.

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/build-phone"
REPORT="$BUILD/selftest-report.txt"

build() {
	cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCLOUD_SAVE=OFF -DMAC_UPDATER=OFF \
		-DPHONE_LAYOUT_SELFTEST=ON -DPHONE_LAYOUT_TESTS=ON >/dev/null
	cmake --build "$BUILD" --target pvz-portable phone-layout-test
}

# name, points wide and high, safe area x,y,w,h (iOS gives both sides the inset of the Dynamic Island, and 21 at the
# bottom for the home indicator)
phone() {
	case "$1" in
		iphone15) echo "852x393 59,0,734,372" ;;
		iphonese) echo "667x375 0,0,667,375" ;;
		iphone15promax) echo "932x430 59,0,814,409" ;;
		*) echo "unknown phone $1" >&2; exit 2 ;;
	esac
}

run() {
	local phones="" levels=""
	for arg in "$@"; do
		case "$arg" in
			iphone*) phones="$phones $arg" ;;
			*) levels="$levels $arg" ;;
		esac
	done
	if [ ! -f "$BUILD/main.pak" ] || [ ! -d "$BUILD/properties" ]; then
		echo "build-phone/ needs main.pak and properties/ from the game." >&2
		exit 2
	fi
	: > "$REPORT"
	local status=0
	for name in ${phones:-iphone15 iphonese iphone15promax}; do
		read -r screen safe <<<"$(phone "$name")"
		echo "=== $name ===" | tee -a "$REPORT"
		rm -rf "$BUILD/selftest-save"
		mkdir -p "$BUILD/selftest-save"
		SDL_AUDIO_DRIVER=dummy PVZP_PHONE_LAYOUT=1 PVZP_SELFTEST=1 PVZP_SELFTEST_SCREEN="$screen" PVZP_SELFTEST_SAFE="$safe" \
			PVZP_SELFTEST_LEVELS="$levels" PVZP_SELFTEST_REPORT="$REPORT" \
			"$BUILD/pvz-portable" -savedir "$BUILD/selftest-save/" >/dev/null 2>"$BUILD/selftest-$name.log" || status=1
		if ! tail -n 1 "$REPORT" | grep -q "checks,"; then
			# the game ended before the test did: say why (no display inside a sandbox, a crash, ...)
			echo "FAIL: the game ended before the test finished:" | tee -a "$REPORT"
			grep -E "ERROR|FATAL" "$BUILD/selftest-$name.log" | sed 's/^.*\] //' | tail -n 3 | tee -a "$REPORT"
			status=1
		fi
		echo | tee -a "$REPORT"
	done
	grep -E "FAIL|checks," "$REPORT" || true
	return $status
}

show() {
	local name=iphone15 level=survival-day
	for arg in "$@"; do
		case "$arg" in
			iphone*) name="$arg" ;;
			*) level="$arg" ;;
		esac
	done
	if [ ! -f "$BUILD/main.pak" ] || [ ! -d "$BUILD/properties" ]; then
		echo "build-phone/ needs main.pak and properties/ from the game." >&2
		exit 2
	fi
	read -r screen safe <<<"$(phone "$name")"
	mkdir -p "$BUILD/show-save"
	SDL_AUDIO_DRIVER=dummy PVZP_PHONE_LAYOUT=1 PVZP_SELFTEST=1 PVZP_SELFTEST_SHOW=1 PVZP_SELFTEST_SCREEN="$screen" \
		PVZP_SELFTEST_SAFE="$safe" PVZP_SELFTEST_LEVELS="$level" \
		"$BUILD/pvz-portable" -savedir "$BUILD/show-save/" >/dev/null 2>&1
}

case "${1:-}" in
	build) build ;;
	show) shift; show "$@" ;;
	run) shift; run "$@" ;;
	*) sed -n '2,17p' "$0"; exit 2 ;;
esac
