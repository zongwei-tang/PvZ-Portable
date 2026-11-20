# PvZ-Portable

<div align="center">
  <img src="icon-readme.png" alt="PvZ-Portable" width="450">
  <br>
  <a href="https://deepwiki.com/wszqkzqk/PvZ-Portable"><img src="https://deepwiki.com/badge.svg" alt="Ask DeepWiki"></a>
</div>

**PvZ-Portable** is a cross-platform, community-driven C++20 reimplementation of *Plants vs. Zombies: Game of the Year Edition*, built to bring an authentic recreation of the classic game to modern desktop, mobile, console, and web platforms.

📦 **Native Builds:** Prebuilt packages for Windows, Linux, macOS, Android, iOS, and Nintendo Switch are available on the [Releases](https://github.com/wszqkzqk/PvZ-Portable/releases) page.

🌐 **Don't want to install?** [Try the WebAssembly build directly in your browser](https://wszqkzqk.github.io/PvZ-Portable/pvz-portable.html) (you still need your own game data files).

> **Important Notice**
> - **Game Assets Required:** This repository does not contain any copyrighted assets (graphics, audio, or fonts) owned by PopCap Games or Electronic Arts. To play, you must supply `main.pak` and the `properties/` folder from a legally purchased copy of *Plants vs. Zombies: GOTY Edition* ([Steam](https://store.steampowered.com/app/3590/Plants_vs_Zombies_GOTY_Edition/) / [EA](https://www.ea.com/games/plants-vs-zombies/plants-vs-zombies)).
> - **Clean-Room Implementation:** The codebase is a manual reimplementation based on publicly documented community research ([植物大战僵尸吧](https://tieba.baidu.com/f?ie=utf-8&kw=%E6%A4%8D%E7%89%A9%E5%A4%A7%E6%88%98%E5%83%B5%E5%B0%B8), [PvZ Wiki](https://wiki.pvz1.com/doku.php?id=home), and [PvZ Tools](https://pvz.tools/memory/)). The author (`wszqkzqk`) has never reverse-engineered the original binary, and code produced directly from reverse engineering is not accepted.
> - **Non-Commercial & Educational:** This project is developed for educational purposes focusing on cross-platform porting, engine modernization, and multi-architecture compatibility. It is not affiliated with, authorized, or endorsed by PopCap Games or Electronic Arts.

---

## Highlights & Features

### Graphics, Audio & Gameplay
- **SDL3 + OpenGL ES 2.0 Backend:** Uses OpenGL ES 2.0 with automatic fallback to desktop OpenGL 2.1+. Because GLES 2.0 is the common subset supported natively by desktop GPU drivers, mobile chips, WebGL, and consoles, the renderer runs out of the box everywhere without extra wrapper dependencies (though [ANGLE](https://chromium.googlesource.com/angle/angle) can still be used to translate calls to DirectX, Metal, or Vulkan if desired).
- **Resizable Window:** Supports free window resizing and fullscreen scaling, lifting the fixed-resolution limitation of the original release.
- **Authentic MO3 Music & Audio:** Powered by `SDL3_mixer` and `libopenmpt` for accurate playback of the original `.mo3` tracker module soundtrack and sound effects.
- **Unlockable Limbo Page:** Restores access to the hidden mini-games page from the original game. To unlock it, open the Mini-Games, Puzzle, or Survival selection screen and click or tap any blank area 5 times in quick succession.
- **Deterministic Replay System:** Record gameplay sessions into compact `.dmo` files and play them back deterministically for debugging or regression testing.

### Portability & Architecture Support
- **32-bit & 64-bit Multi-Architecture:** Runs natively on `i686`, `x86_64`, `aarch64`, `riscv64`, `loongarch64`, and more.
- **Endian-Independent Design:** Supports both little-endian and big-endian systems, including cross-endian save compatibility (big-endian support is implemented by design, though untested on physical hardware).
- **Modern C++20 Foundation:** Built with `std::thread`, `std::filesystem`, unified internal UTF-8 string handling, and full Unicode file path support across all operating systems.
- **Low-Memory Mode:** Optional `LOW_MEMORY` build mode disables resource pre-caching for RAM-constrained console targets.

### Save & Asset Compatibility
- **Original Profile Compatibility:** Directly reads and writes original PC GOTY global profile files (`users.dat`, `user*.dat` covering adventure progress, coins, Zen Garden, achievements, etc.), while fixing the Year 2038 timestamp overflow bug in a backward-compatible manner.
- **Cross-Platform Mid-Level Saves (`.v4`):** Replaces the original game's fragile raw-memory `.dat` dumps with a structured TLV save format that can be freely transferred across operating systems, compilers, and 32/64-bit CPU architectures, complete with a YAML export/import tool.
- **Cloud Save:** Keeps saves in sync through WebDAV, Google Drive or OneDrive on desktop systems — see [Cloud Save](#cloud-save).
- **Multilingual Asset Support:** Works with official GOTY asset packages in English, Chinese, German, Spanish, French, and Italian.

---

## Supported Platforms

| Platform | Resource & Data Location | Status |
| :--- | :--- | :--- |
| **Linux** | Executable dir (resources); per-user app-data (saves/caches) | Fully supported |
| **Windows** | Executable dir (resources); per-user app-data (saves/caches) | Fully supported |
| **macOS** | Executable dir (resources); per-user app-data (saves/caches) | Fully supported |
| **BSD Family** | Executable dir (resources); per-user app-data (saves/caches) | Supported (verified on FreeBSD) |
| **Haiku** | Executable dir (resources); per-user app-data (saves/caches) | Partially works (no music) |
| **Android** | `Android/data/io.github.wszqkzqk.pvzportable/files/` | Fully supported |
| **iOS / iPadOS** | App Documents directory (via iOS Files app / Finder) | Supported (sideload via unsigned IPA) |
| **Web (WASM)** | Browser IndexedDB (saves); resources loaded locally at runtime | Fully supported (requires HTTP server) |
| **Nintendo Switch** | `sdmc:/switch/PvZPortable` | Works on real hardware (Kenji-NX crashes on boot) |

---

## Getting Started

### Game Assets & Version Compatibility

You need `main.pak` and the `properties/` folder from a legal copy of *Plants vs. Zombies: GOTY Edition* (unpacked resource files can also be used instead of `main.pak`).

PvZ-Portable is designed and tested primarily against **GOTY Edition 1.2.0.1073 (English, standalone PopCap release)**, which is recommended for the best out-of-the-box experience.

Other official releases, including **Steam GOTY 1.2.0.1096** and **non-English GOTY editions** (`1.2.0.1093` DE/ES/FR/IT, or `1.1.0.1056` ZH based on 1.2.0.1073), are also fully playable with identical gameplay mechanics. Because those versions renamed a few XML string keys, you may notice minor UI text differences unless you generate a compatibility override file:

```bash
python3 scripts/make-compat-overrides.py "/path/to/main.pak"
```

This script generates `pvz-portable.xml` inside the `properties/` directory next to `main.pak` (use `-o` to specify a custom output path, or `--force` to overwrite an existing file).

<details>
<summary>Known cosmetic text differences on non-1.2.0.1073 EN assets without overrides (click to expand)</summary>

| Issue (Non-1.2.0.1073 EN only) | Cause |
| :--- | :--- |
| **Almanac blue description text missing** | In 1.2.0.1096, the introductory paragraph was split from `[XXX_DESCRIPTION]` into `[XXX_DESCRIPTION_HEADER]`, which the 1.2.0.1073 engine does not read. |
| **"Restart" button label missing** | `[RESTART_LEVEL]` was renamed to `[RESTART_LEVEL_BUTTON]` in 1.2.0.1096. |
| **Unencountered zombie shows `???`** instead of `(not encountered yet)` | `[NOT_ENCOUNTERED_YET]` was changed to `???` in 1.2.0.1096, moving the old text to `[NOT_ENCOUNTERED_YET_DESCRIPTION]`. |
| **Crazy Dave's plant sell price shows 1/10 of actual value** | In 1.2.0.1073, `[CRAZY_DAVE_1700]` appends a literal `0` after `{SELL_PRICE}` (`${SELL_PRICE}0`) because the engine passes `price / 10`. In 1.2.0.1096 that trailing `0` was removed. |
</details>

### Desktop (Linux, Windows, macOS, BSD, etc.)

Place `main.pak` and the `properties/` folder in the same directory as the `pvz-portable` executable. By default, the game resolves read-only resources relative to the executable directory, so you can launch it from any working directory.

Per-user writable files are stored separately in your operating system's standard application data directory under `io.github.wszqkzqk/PvZPortable`:
- **Linux:** `~/.local/share/io.github.wszqkzqk/PvZPortable/`
- **Windows:** `%APPDATA%\io.github.wszqkzqk\PvZPortable\`
- **macOS:** `~/Library/Application Support/io.github.wszqkzqk/PvZPortable/`

Inside this directory, the game maintains:
- `userdata/`: Player profiles and mid-level save files.
- `cache64/` (64-bit builds) or `cache32/` (32-bit builds): Local startup caches for compiled reanimation and definition data. These use native memory layout and are not meant to be shared across machines; if a cache or schema check fails, the game automatically recompiles them from source assets.
- `registry.regemu`: Persisted game settings.

### Android

Download the prebuilt `arm64-v8a` APK from [Releases](https://github.com/wszqkzqk/PvZ-Portable/releases) (or build from source for other ABIs). Requires Android 7.0+.

1. **First Launch:** Install and open the APK. When no assets are detected, the resource import screen opens automatically.
2. **Import Assets:** Select either:
   - A ZIP archive containing `main.pak` and `properties/` (at the archive root or inside a single top-level folder such as `PvZ/main.pak`), or
   - A directory containing `main.pak` and `properties/`.
3. Tap **Start Game** once import completes.
4. **Managing Data Later:** Long-press the app icon in your launcher and select **Manage Data** to re-import resources or import/export save files.

*Notes:*
- All files are stored under `Android/data/io.github.wszqkzqk.pvzportable/files/`. Imports and exports use the Android Storage Access Framework (SAF), so no broad storage permissions are required.
- The Android build preserves the desktop game's 4:3 aspect ratio and mouse-driven interaction model via SDL3 touch-to-mouse mapping, without mobile-specific UI redesigns.

### iOS / iPadOS

Download the unsigned IPA from [Releases](https://github.com/wszqkzqk/PvZ-Portable/releases) or build it via `ios/build-ios.sh`. Requires iOS 16.4+ (`arm64`).

1. **Sideload the IPA:** Install using [AltStore](https://altstore.io/), [TrollStore](https://github.com/opa334/TrollStore), or Xcode with a free Apple ID (note that free Apple ID signatures expire after 7 days, whereas TrollStore installations are permanent).
2. **Copy Game Assets:** Open the iOS **Files** app (or Finder / iTunes file sharing on desktop) and copy `main.pak` and the `properties/` folder into the **PvZ Portable** Documents directory.

### Web (Browser / WebAssembly)

1. Open the [Online WebAssembly Player](https://wszqkzqk.github.io/PvZ-Portable/pvz-portable.html).
2. Load your game assets from either a ZIP archive or a folder containing `main.pak` and `properties/`, then click **Start Game**.
   - All assets are processed locally in your browser and are never uploaded to any server.
   - Saves are stored in browser IndexedDB and can be exported or imported as a ZIP archive using the on-screen controls.

To self-host the WebAssembly build, download the WASM archive from [Releases](https://github.com/wszqkzqk/PvZ-Portable/releases) and serve it over HTTP (for example, `python3 -m http.server`). Opening the `.html` file directly via `file://` will not work due to browser security restrictions.

---

## Save Data & Conversion Tools

PvZ-Portable uses two types of save files inside `userdata/`:

1. **Global User Data (`users.dat`, `user1.dat`, etc.)**
   - Stores player profiles, adventure progress, coins, Zen Garden plants, and shop purchases.
   - Uses explicit field serialization and is directly compatible with the original PC GOTY Edition across all platforms.

2. **Mid-Level Save States (`game1_0.v4`, etc.)**
   - Created when you choose "Save and Exit" during a level.
   - **`.v4` (Default, Portable):** Serializes game objects field-by-field using Type-Length-Value (TLV) tags. You can freely copy `.v4` files between Windows, Linux, macOS, Android, iOS, Web, and Switch across 32/64-bit and different CPU architectures.
   - **`.dat` (Legacy):** The original game's mid-level save format dumped raw C++ structs directly to disk, which breaks across different architectures, pointer sizes, or compilers. PvZ-Portable still loads legacy `.dat` saves as a fallback and automatically migrates them to `.v4`.

### Inspecting & Editing `.v4` Saves

The repository includes `scripts/pvzp-v4-converter.py` for inspecting mid-level saves or converting them to and from human-readable YAML:

```bash
# View basic level statistics (wave, sun, plant/zombie counts, etc.)
python3 scripts/pvzp-v4-converter.py info ~/.local/share/io.github.wszqkzqk/PvZPortable/userdata/game1_13.v4

# Export a binary .v4 save to YAML for inspection or editing
python3 scripts/pvzp-v4-converter.py export ~/.local/share/io.github.wszqkzqk/PvZPortable/userdata/game1_13.v4 level.yaml

# Re-import modified YAML back into a .v4 save (back up your original file first!)
cp ~/.local/share/io.github.wszqkzqk/PvZPortable/userdata/game1_13.v4 ~/.local/share/io.github.wszqkzqk/PvZPortable/userdata/game1_13.v4.bak
python3 scripts/pvzp-v4-converter.py import level.yaml ~/.local/share/io.github.wszqkzqk/PvZPortable/userdata/game1_13.v4
```

---

## Cloud Save

Desktop builds can keep the `userdata/` folder (profiles, adventure progress, coins, Zen Garden and mid-level saves) in sync with the cloud, so you can continue on another computer. Three services are supported:

- **WebDAV**: any WebDAV server, e.g. Nextcloud, ownCloud, Synology, Apache `mod_dav` or Jianguoyun (坚果云).
- **Google Drive**: the saves go into the app's hidden data folder in your Drive, where they do not mix with your files.
- **OneDrive**: the saves go into `Apps/<app name>/PvZPortable/` in your OneDrive.

Set it up in the game: on the main menu open **Options → Cloud Save** and pick the service with the **Service** button. The Cloud Save button is only on the main menu, because a sync can replace the loaded profiles.

### WebDAV

Tick **Enabled**, enter the server URL, username and password, then press **Sync Now** to try the connection, or **Save**.

- The server URL is the WebDAV root of your account; the saves go into the folder `PvZPortable` below it.
- The password is stored as **plain text**. Use an app password (Nextcloud, Jianguoyun and most others offer them) rather than your account password.

### Google Drive and OneDrive

These sign in through the browser instead of with a password:

1. Pick the service, tick **Enabled** and press **Sign In**. A build without a client of its own for the service also shows fields for the client ID (for Google also the client secret, see below); fill them in first.
2. The game opens the sign-in page in your browser, and copies its address to the clipboard in case no page opens. Sign in there and allow the access. The browser then comes back to the game by itself and says when you can close the tab. It has to be a browser on the same computer, because it comes back to an address that only this computer can reach.
3. Once the dialog shows your account, press **Save**.

If the sign-in page shows an error instead, see [Sign-in problems](#sign-in-problems). The game waits for the browser for up to 10 minutes; **Stop Sign-In** ends the wait sooner.

The sign-in is kept in `cloudsave-google-token.txt` or `cloudsave-onedrive-token.txt` in the per-user data folder (on macOS and Linux readable only by your user account). **Sign Out** forgets it on this computer; to revoke the access itself, remove the app in your Google or Microsoft account settings. **Cancel** also takes back the settings that **Sign In** saved, but keeps the sign-in for when you save them. Settings that **Sync Now** saved stay, as with **Save**. **Cancel** in a later visit to the dialog does not stop a sign-in that went on after **Save**; **Stop Sign-In** does.

A sign-in works only with the client it was made with: after you enter another client ID, or a new build brings another built-in one, the game asks you to sign in again. A sign-in kept by an earlier version of the game does not say which client it was made with; see [Sign-in problems](#sign-in-problems) if the game says it was made with another client ID.

A build has no client of its own unless whoever made it [built one in](#built-in-client-ids); then leave the client fields empty. Otherwise create your own, which is free:

#### Google Cloud client

1. In the [Google Cloud console](https://console.cloud.google.com/), create a project and enable the **Google Drive API** for it.
2. Set up the OAuth consent screen (Google Auth Platform): audience **External**, any app name, and under **Data Access** add the scope `https://www.googleapis.com/auth/drive.appdata`.
3. Under **Audience**, press **Publish app**, so that the app is **In production**. While it is in **Testing**, only the test users you add there can sign in, and Google ends their sign-in after 7 days, after which the game asks you to sign in again. `drive.appdata` is not a sensitive scope, so a published app needs no verification by Google.
4. Under **Clients** (**Credentials** in the older console), create an OAuth client of the type **Desktop app**; it needs no redirect URI. Enter its client ID and client secret in the game. Copy the secret right away, or download the JSON: Google shows it only when the client is created. Google requires the secret for this kind of sign-in, even though a program on your computer cannot keep it secret.

A client of the type **TVs and Limited Input devices**, which earlier versions of the game used, is meant for the old sign-in with a code: create a **Desktop app** client for this one and enter its ID and secret instead. Then press **Sign Out** and **Sign In**, since a sign-in made with the old client does not work with the new one.

#### Microsoft app registration

1. In the [Microsoft Entra admin center](https://entra.microsoft.com/) (or the Azure portal), open **App registrations → New registration**:
   - **Name**: becomes the folder under `Apps/` in OneDrive, e.g. `PvZ Portable`.
   - **Supported account types**: **Accounts in any organizational directory and personal Microsoft accounts**. The game signs in through the `common` endpoint, which turns down a registration for personal accounts only.
   - **Redirect URI**: the platform **Public client/native (mobile & desktop)** and the address `http://localhost`, exactly so: no port, no path, no `/` at the end. Microsoft ignores the port of a localhost address, so the game can listen on any free port.
2. For a registration you already have, open **Authentication**, choose **Add a platform → Mobile and desktop applications** and add `http://localhost` as a custom redirect URI. Do not add it under **Web**: Microsoft then takes the game for a program that keeps a secret, and the sign-in fails with `AADSTS7000218`. **Allow public client flows**, which earlier versions of the game needed, is not needed any more.
3. Under **API permissions**, add the Microsoft Graph delegated permissions `Files.ReadWrite.AppFolder` and `offline_access`.
4. Enter the **Application (client) ID** from the **Overview** page in the game. There is no client secret.

Microsoft documents `Files.ReadWrite.AppFolder` for personal Microsoft accounts and marks it as preview, so a work or school account may not be able to use it, or may need its administrator's consent.

#### Sign-in problems

If the sign-in page shows an error, the browser cannot come back to the game: press **Stop Sign-In**, fix the client and sign in again. Other errors do come back, and the game shows them. What the common ones mean:

- Google **`redirect_uri_mismatch`**, **`invalid_request`** or **`invalid_client`**: the client is not of the type **Desktop app** (it is a web application or TV client, say), or the client ID is wrong.
- Google says the access is blocked while the app is in **Testing**: the account is not one of the app's test users. Add it under **Audience**, or publish the app.
- **"Google did not accept the client secret"**, when you sign in, or **"Google did not accept the client ID and secret"**, while syncing: the secret is not one of this client's, or was deleted from it. Google shows a secret only when it is made: under **Clients**, open the client, add a new secret and enter that one.
- **"the Google Drive sign-in was made with another client ID"**: an earlier version of the game signed in with another client, such as a **TVs and Limited Input devices** one. Press **Sign Out**, then **Sign In**.
- Microsoft **`AADSTS50011`** or **`AADSTS500113`**: `http://localhost` is not a redirect URI of the registration.
- Microsoft **`AADSTS7000218`** or **`AADSTS9002327`**: `http://localhost` is under **Web** or **Single-page application**. Remove it there and add it under **Mobile and desktop applications**.
- Microsoft **`AADSTS700016`**: the application (client) ID is wrong.
- Microsoft **`AADSTS50194`** or **`AADSTS9002331`**: the supported account types are not the ones above. A new registration with the right ones is the simplest fix.
- **"the game was not given access"**: you declined, or for Google left the box for the game's data unticked. Sign in again and allow it.

Two things on your computer can get in the way:

- If the browser turns `http://localhost:…` (OneDrive) into `https://` and shows an error, a site you ran on `localhost` earlier told it to always use https there (HSTS). In Chrome or Edge, delete the domain security policy of `localhost` at `chrome://net-internals/#hsts` (`edge://net-internals/#hsts`); in Firefox, use **Forget About This Site** on `localhost` in the history. Google comes back to `http://127.0.0.1` instead, and HSTS never applies to an IP address.
- If a firewall asks whether PvZ-Portable may accept connections while you sign in, that is the game waiting for the browser. It listens only on this computer's loopback address, which other computers cannot reach. Allow it if the browser does not come back.

#### Built-in client IDs

To make a build that signs in without these steps, pass the client IDs, and the secret of the Google client, to the first `cmake` command; they are compiled into the game. CMake stops if only one of the Google client ID and secret is given:

```bash
cmake -G Ninja -B build -DCLOUD_SAVE_GOOGLE_CLIENT_ID=1234-abc.apps.googleusercontent.com -DCLOUD_SAVE_GOOGLE_CLIENT_SECRET=GOCSPX-abc -DCLOUD_SAVE_ONEDRIVE_CLIENT_ID=00000000-0000-0000-0000-000000000000
```

The dialog then shows no client fields for that service: players only press **Sign In**. A client ID in `cloudsave.ini` still takes precedence over the built-in one, and a sign-in made with the one does not count with the other.

### The settings file

The settings are kept in `cloudsave.ini` next to `registry.regemu` in the per-user data folder, which you can also edit by hand and then restart the game:

```ini
enabled=1
provider=webdav
url=https://cloud.example.com/remote.php/dav/files/alice/
username=alice
password=app-password-here
folder=PvZPortable
google_client_id=
google_client_secret=
onedrive_client_id=
```

- `provider` is `webdav`, `google` or `onedrive`.
- `folder` holds the saves: below `url` on WebDAV, in the app's data folder on Google Drive, in the app's folder on OneDrive.

### How syncing works

- At start the game downloads newer saves from the cloud before loading the profiles.
- When saves change, they are uploaded in the background a few seconds later, and once more when the game exits. Every 5 minutes the game also checks the cloud for saves from other devices; those are loaded the next time the main menu is idle.
- The folder is synced as a whole, never file by file, because profile ids differ between devices.
- If the saves changed both on this device and in the cloud since the last sync, the main menu asks which copy to keep (click a button; the keyboard is ignored there so a stray key cannot decide). The other copy is backed up to `cloudsave-backup/` in the per-user data folder. Keeping this device's copy also repairs a cloud copy that is incomplete.
- OneDrive and most WebDAV servers turn down an upload that would overwrite another device's newer save. Google Drive cannot, so when two devices save at the same moment there, the game notices on the next sync and asks which copy to keep.
- After switching to another service, folder or account, the first sync asks which copy to keep if both this device and the cloud already have different saves.
- Without a connection the game runs normally and retries later.
- Cloud files: `manifest.txt` is the committed state and `files/` holds the save files. Do not edit them by hand.

To test the protocol without any server, configure with `-DCLOUD_SAVE_TESTS=ON` and run `build/cloudsync-test`. Its tests of the sign-in listener and of stopping a request need to listen on `127.0.0.1`. They are skipped where listening is not allowed, as in some sandboxes, and fail on any other error.

## Command-Line Arguments

Both `-flag <value>` and `-flag=<value>` syntaxes are supported:

### Path Overrides
- `-resdir <path>`: Set the directory where read-only game assets (`main.pak` and `properties/`) are located.
- `-savedir <path>`: Override the directory used for writable data (saves, caches, settings, and screenshots).

### Recording and Playing Back Gameplay
Demo files (`.dmo`) record deterministic input and RNG state for debugging and regression testing. Automatically named recordings are written to the current working directory:
- `-record [<path>]`: Record the session to `pvzp-YYYYMMDD-HHMMSS[-N].dmo`, or to `<path>` if specified.
- `-recnum <N>`: Record a new session and retain only the `N` most recent auto-named `pvzp-*.dmo` files (custom-named files are untouched).
- `-play [<path>]`: Replay the most recent `pvzp-*.dmo` file, or `<path>` if specified.
- `-playnum <N>`: Replay the `N`-th most recent auto-named `pvzp-*.dmo` recording (`1` is the latest).

---

## Building from Source

### Dependencies

- **Build System:** `CMake` (>= 3.12), `Ninja`
- **Compiler:** C++20-capable compiler and standard library (`GCC`, `Clang`, or `MSVC`)
- **Libraries:** `SDL3` (>= 3.2.12), `SDL3_mixer`, `libopenmpt`, `libpng`, `libjpeg-turbo`, `zlib`
- **Graphics API:** `OpenGL ES 2.0` or `OpenGL 2.1+` headers/drivers
- **Cloud save** (optional): `libcurl`. Without it the game builds without cloud save.

Install dependencies for your platform:

- **Arch Linux:**
  ```bash
  sudo pacman -S --needed base-devel cmake curl libjpeg-turbo libopenmpt libpng ninja sdl3 sdl3_mixer
  ```
- **Debian / Ubuntu:**
  ```bash
  sudo apt install cmake ninja-build libcurl4-openssl-dev libjpeg-dev libopenmpt-dev libpng-dev libsdl3-dev libsdl3-mixer-dev
  ```
- **Windows (MSYS2 UCRT64):**
  ```bash
  pacman -S --needed base-devel mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-curl mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-libjpeg-turbo mingw-w64-ucrt-x86_64-libopenmpt mingw-w64-ucrt-x86_64-libpng mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-sdl3 mingw-w64-ucrt-x86_64-sdl3-mixer
  ```
- **macOS (Homebrew):**
  ```bash
  brew install cmake dylibbundler jpeg-turbo libopenmpt libpng ninja sdl3 sdl3_mixer
  ```

### Compiling

Configure and build from the repository root:

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

For local native builds, you can also enable CPU-specific instruction sets via compiler flags:

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_FLAGS="-march=native" -DCMAKE_CXX_FLAGS="-march=native"
```

### Mac app

On macOS, one command builds the game as an app and installs it to `/Applications/PlantsVsZombies.app`, replacing the one there. It installs missing [Homebrew](https://brew.sh/) packages and bundles the libraries, so the app runs without Homebrew:

```bash
./macos/build-app.sh
```

The app carries no game data, so that an update can replace it whole: `main.pak` and `properties/` live in the per-user data folder, next to the saves (the script prints where). The script copies them there from the app it replaces (also from apps made by its earlier versions, which carried them inside); without them, pass the folder that holds them: `./macos/build-app.sh --data /path/to/data`. With `--no-install` it only builds `build-mac/PlantsVsZombies.app`. Quit the game before installing.

#### Releases and updates

Pushing a tag `v<version>` (e.g. `v0.2.7.2`) runs [`.github/workflows/ci.yml`](.github/workflows/ci.yml), which builds every platform (Linux, Windows, macOS, Android, iOS, WebAssembly) and puts all of them into the release of the tag. For the Mac app it calls [`.github/workflows/mac-release.yml`](.github/workflows/mac-release.yml), which builds the app and signs it for updates, so the release also holds `PlantsVsZombies-mac.zip` and `appcast.xml`. In a fork, the AUR and GitHub Pages jobs of upstream are skipped. The release needs the repository secret `SPARKLE_PRIVATE_KEY`, the private key that goes with `macos/sparkle-public-key.txt`.

The app updates itself through [Sparkle](https://sparkle-project.org/): about once a day it looks at the latest release of the repository of the remote `origin`, and when that has a newer build it shows a window with **Install Update**, which downloads the update, checks its signature, replaces the app and starts it again. **Options → Check Updates** on the main menu looks right away. The build number is the number of commits, so every release built from a later commit counts as newer.

An app downloaded from a release is signed ad hoc, not by an Apple developer ID, so macOS blocks it the first time: open it with right-click → **Open**, or allow it in **System Settings → Privacy & Security**. A downloaded app has no game data yet: put `main.pak` and `properties/` into the per-user data folder; the game names that folder when it cannot find them.

The Google client of this project is built in by default. To build in other [OAuth clients](#built-in-client-ids), put them into `macos/cloud-clients.cmake`, which is not in git:

```cmake
set(CLOUD_SAVE_GOOGLE_CLIENT_ID "1234-abc.apps.googleusercontent.com" CACHE STRING "" FORCE)
set(CLOUD_SAVE_GOOGLE_CLIENT_SECRET "GOCSPX-abc" CACHE STRING "" FORCE)
set(CLOUD_SAVE_ONEDRIVE_CLIENT_ID "00000000-0000-0000-0000-000000000000" CACHE STRING "" FORCE)
```

### CMake Configuration Options

Pass `-D<OPTION>=ON/OFF` during the `cmake -B build` step to customize the build:

| Option | Default | Description |
| :--- | :--- | :--- |
| `PVZ_DEBUG` | `OFF` (`ON` in `Debug` builds) | Enable in-game cheat keys, debug overlays, and diagnostic features. |
| `DO_FIX_BUGS` | `OFF` | Apply optional community fixes for original 1.2.0.1073 gameplay quirks and bugs.[^1] Disabled by default because many players consider original behaviors part of classic gameplay. |
| `LOW_MEMORY` | `OFF` | Reduce memory footprint by disabling resource pre-caching (intended for consoles with limited RAM). |
| `CONSOLE` | `OFF` (`ON` in `Debug` builds) | Attach a stdout/stderr console window on Windows. |
| `BUILD_STATIC` | `OFF` | Statically link dependencies on Windows (MinGW) and glibc-based Linux. For MSVC, use a vcpkg `-static` triplet instead. |
| `CLOUD_SAVE` | `ON`<br>(`OFF` on WebAssembly, Switch, Android, iOS and with `BUILD_STATIC`) | Build the [cloud save](#cloud-save) to WebDAV, Google Drive and OneDrive. Needs `libcurl`; when it is not found the game builds without cloud save. |
| `CLOUD_SAVE_GOOGLE_CLIENT_ID`<br>`CLOUD_SAVE_GOOGLE_CLIENT_SECRET` | the client of this project | ID and secret of a Google OAuth client of the type **Desktop app**, built into the game so players can sign in to Google Drive without [their own client](#google-cloud-client). Give both or neither. |
| `CLOUD_SAVE_ONEDRIVE_CLIENT_ID` | empty | ID of a Microsoft app registration with the redirect URI `http://localhost` under **Mobile and desktop applications**, built into the game so players can sign in to OneDrive without [their own registration](#microsoft-app-registration). |
| `CLOUD_SAVE_TESTS` | `OFF` | Also build `cloudsync-test`, offline tests of the cloud save against simulated WebDAV, Google Drive and OneDrive servers. |

[^1]: `DO_FIX_BUGS` currently includes the following adjustments:
    - Fix Bungee Zombie duplicate sun/item drop in *I, Zombie* mode.
    - Make mind-controlled Gargantuars smash enemy zombies instead of plants, smash vases in *Scary Potter* mode, and throw mind-controlled Imps (with corrected scale, health, and direction).
    - Make mind-controlled Pea/Gatling Head zombies shoot forward toward enemy zombies instead of backward.
    - Make mind-controlled Jalapeno and Squash zombies target and damage enemy zombies instead of plants (including coordinate fixes for Squash tracking).
    - Allow mind-controlled Jalapeno zombies to clear Dr. Zomboss's ice/fireballs and ladder placements properly.
    - Synchronize Backup Dancer animations with the Dancing Zombie (fixes the "Maid" displacement glitch).
    - Fix visual glitch when a Ladder Zombie recovers its arm.

For example, to build an optimized Release binary with cheat keys enabled:

```bash
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=Release -DPVZ_DEBUG=ON
```

---

## Contributing

Contributions are welcome! If you encounter build or gameplay issues, please open an issue with your platform and reproduction details.

### Developer Notes

- **Format v4 Save Compatibility:**
  - Always assign new TLV IDs when introducing new serialized fields; never reuse existing IDs.
  - Provide sensible default values when loading older `.v4` saves that lack newly added fields.
  - Never dump raw C++ structs for layout-dependent types; serialize fields explicitly using fixed-width primitives.

<details>
<summary>SexyAppFramework Coding Conventions (click to expand)</summary>

From the original SexyAppFramework documentation:

> The framework differs from many other APIs in that some class properties are not wrapped in accessor methods, but rather are made to be accessed directly through public member data. The window caption of your application, for example, is set by assigning a value to the `std::string mTitle` in the application object before the application's window is created. We felt that in many cases this reduced the code required to implement a class. Also of note is the prefix notation used on variables: `m` denotes a class member, `the` denotes a parameter passed to a method or function, and `a` denotes a local variable.
</details>

---

## License & Acknowledgments

**Copyright (C) 2026 Zhou Qiankang `<wszqkzqk@qq.com>`**

This project's code is licensed under the [**GNU Lesser General Public License v3.0 or later**](https://www.gnu.org/licenses/lgpl-3.0.html) (`LGPL-3.0-or-later`). Full license texts are provided in `LICENSE` (LGPL-3.0) and `COPYING` (GPL-3.0) at the repository root. The code is provided "as is", without warranty of any kind.

- **Intellectual Property:** *Plants vs. Zombies* and all original game assets are the property of PopCap Games and Electronic Arts. The LGPL-3.0-or-later license applies solely to the source code in this repository. Project icons and platform logos were created by `wszqkzqk` with the assistance of AI image generation tools and are not official PopCap/EA assets.
- **PopCap Games Framework:** Portions of `src/SexyAppFramework` are derived from the [PopCap Games Framework](src/SexyAppFramework/LICENSE) and have been refactored and modernized by the community under the LGPL-3.0-or-later license:
  > *"This product includes portions of the PopCap Games Framework, © 2005-2009 PopCap Games, Inc. All rights reserved. (http://popcapframework.sourceforge.net/)."*

### Acknowledgments
- **PopCap Games** for creating *Plants vs. Zombies* and releasing the SexyAppFramework under a permissive license.
- **The SDL Team** for SDL3 and SDL3_mixer, which power the cross-platform windowing, input, rendering, and audio backends.
- **The OpenMPT Team** for `libopenmpt`, enabling accurate MO3 tracker music playback.
- All community contributors and researchers, especially [@Headshotnoby](https://github.com/headshot2017) and [@Patoke](https://github.com/Patoke) for their foundational work.
