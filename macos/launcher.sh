#!/bin/bash
cd "$(dirname "$0")"

# Keep SDL from handing focus to a separate macOS fullscreen Space or
# minimizing the game when focus briefly changes during level transitions.
export SDL_VIDEO_MAC_FULLSCREEN_SPACES=0
export SDL_VIDEO_MINIMIZE_ON_FOCUS_LOSS=0
export SDL_MOUSE_FOCUS_CLICKTHROUGH=1
export SDL_MAC_MOUSE_FOCUS_CLICKTHROUGH=1

pref_dir="${HOME}/Library/Application Support/io.github.wszqkzqk/PvZPortable"
reg_file="${pref_dir}/registry.regemu"
if [ -f "$reg_file" ] && perl -0ne 'exit(/ScreenMode\x00\x04\x00\x00\x00\x04\x00\x00\x00\x00\x00\x00\x00/ ? 0 : 1)' "$reg_file"; then
    [ -f "${reg_file}.bak.before-window-fix" ] || cp -p "$reg_file" "${reg_file}.bak.before-window-fix"
    perl -0777 -i -pe 's/(ScreenMode\x00\x04\x00\x00\x00\x04\x00\x00\x00)\x00\x00\x00\x00/${1}\x01\x00\x00\x00/' "$reg_file"
fi

exec ./pvz-portable "$@"
