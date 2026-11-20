#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# Writes altstore-source.json, the AltStore source of one release: AltStore and SideStore install and update the iOS
# app from it, and the app looks at it for updates (src/Lawn/System/AppUpdater.h). Run by the CI on a tag:
#
#   altstore-source.py <app>/Info.plist <ipa> <out.json>
#
# with GITHUB_REPOSITORY (owner/name) and GITHUB_REF_NAME (the tag) set. The version fields come from the built app, so
# they are exactly those AltStore compares with the installed app.

import datetime
import json
import os
import plistlib
import sys


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: altstore-source.py <Info.plist> <ipa> <out.json>")
    info_path, ipa, out = sys.argv[1:4]
    with open(info_path, "rb") as f:
        info = plistlib.load(f)
    repo = os.environ["GITHUB_REPOSITORY"]
    tag = os.environ["GITHUB_REF_NAME"]
    base = f"https://github.com/{repo}"

    # AltStore checks that the source declares what the app asks for: no entitlements (the IPA is unsigned), and the
    # usage descriptions of its Info.plist
    privacy = {key: value for key, value in info.items() if key.startswith("NS") and key.endswith("UsageDescription")}
    app = {
        "name": "PvZ Portable",
        "bundleIdentifier": info["CFBundleIdentifier"],
        "developerName": "PvZ-Portable contributors",
        "subtitle": "The game on iOS, with cloud save",
        "localizedDescription": "Needs main.pak and properties/ from your own copy of the game: copy them into the "
                                "PvZ Portable folder in the Files app.",
        "versions": [{
            "version": info["CFBundleShortVersionString"],
            "buildVersion": str(info["CFBundleVersion"]),
            "date": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "localizedDescription": f"Release {tag}: {base}/releases/tag/{tag}",
            "downloadURL": f"{base}/releases/download/{tag}/pvz-portable-ios-arm64.ipa",
            "size": os.path.getsize(ipa),
            "minOSVersion": info.get("MinimumOSVersion", "16.4"),
        }],
        "appPermissions": {"entitlements": [], "privacy": privacy},
    }
    icon = os.environ.get("ALTSTORE_ICON_URL", "")
    if icon:
        app["iconURL"] = icon
    source = {
        "name": f"PvZ Portable ({repo.split('/')[0]})",
        "subtitle": "Builds of this repository",
        "website": base,
        "apps": [app],
        "news": [],
    }
    with open(out, "w", encoding="utf-8") as f:
        json.dump(source, f, indent=2)
        f.write("\n")


if __name__ == "__main__":
    main()
