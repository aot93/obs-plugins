# obs-plugins

Custom plugins for [OBS Studio](https://github.com/obsproject/obs-studio), built against
libobs' [source plugin API](https://docs.obsproject.com/reference-sources).

## Plugins

### obs-datetime-source

Adds a **"Date/Time Text"** source that renders the current system date and/or time
directly onto the canvas, refreshed once per second.

Add the source twice (e.g. once set to "Date Only", once to "Time Only") to get
independently-positioned date and time overlays, as in the reference screenshot — no
need for two separate plugins.

Properties:

- **Display Mode** — Date Only, Time Only, Date & Time, or Custom Format
- **Format String** — a C `strftime()` format string, editable when Display Mode is
  "Custom Format" (e.g. `%A, %d %B %Y`)
- **Font** — family, size, bold/italic
- **Text Color** — with alpha
- **Show Background** / **Background Color** — optional solid background plate behind
  the text

Text is rasterized with [FreeType2](https://freetype.org/) into an RGBA texture, so it
renders identically across platforms. Font family/style lookup (turning "Sans Bold"
into an actual `.ttf`/`.otf` file) currently uses:

- **Linux** — [fontconfig](https://www.freedesktop.org/wiki/Software/fontconfig/) (fully
  supported)
- **Windows / macOS** — a small hardcoded fallback to a common system font (see
  `src/font-resolver.cpp`); proper DirectWrite / CoreText family resolution is not yet
  implemented

### obs-ltc-source

Adds an **"LTC Timecode Source"** that decodes [Linear Timecode](https://en.wikipedia.org/wiki/Linear_timecode)
from the audio of another OBS source (e.g. a capture card feed, a media file, or a
mic input carrying an LTC signal) and renders it on the canvas as `HH:MM:SS:FF`.

Properties:

- **Audio Source** — pick any existing OBS source that produces audio; the plugin
  attaches an audio-capture callback to it and feeds the raw samples to
  [libltc](https://github.com/x42/libltc) for decoding
- **Display Style** — `Segmented (Classic LED Clock)` (default) or `Plain Text`
- **Segmented style**:
  - **Segment Color (Lit)** / **Segment Color (Unlit)** — with alpha; unlit segments
    are drawn dim by default (like a real LED/LCD display showing its unpowered
    segments), toggle off with **Show Unlit Segments**
  - **Digit Height** and **Segment Thickness** — size and boldness of the digits
- **Plain Text style**: **Font** (family, size, bold/italic) and **Text Color**
- **Show Background** / **Background Color** — optional solid background plate behind
  the text (applies to both styles)
- **Auto Start/Stop Recording with Timecode** — when enabled, drives OBS's recording
  start/stop directly from the incoming timecode:
  - **Min. Consecutive Frames to Start** — the timecode must be seen advancing for this
    many consecutive decoded frames before recording is started, to guard against a
    handful of garbled/spurious decodes (e.g. signal noise) falsely triggering a start
  - **Wait Before Stopping (seconds)** — once recording, how long the timecode must be
    frozen or absent before recording is stopped, to ride through a brief signal
    dropout or dropped LTC frame without prematurely cutting the recording
  - **Ignore Timecode Ranges** — comma-separated `HH:MM:SS:FF-HH:MM:SS:FF` windows
    (both ends inclusive) where the trigger must never fire, e.g.
    `22:00:00:00-23:00:00:00, 23:30:00:00-23:45:00:00` to skip a pre-show slate or
    test sequence. While the current timecode falls inside one of these ranges it's
    treated like a lost/frozen signal: it can't start a new recording, and a
    recording already running winds down on the normal "Wait Before Stopping"
    schedule instead of continuing
  - Recording is controlled via `obs-frontend-api`, resolved dynamically at runtime
    (see `src/frontend-recording.cpp`) rather than linked at build time — this feature
    only works when the plugin is actually running inside a full OBS Studio
    application, not a headless `libobs`-only host

Digits are drawn as genuine seven-segment shapes (solid rectangles composited
directly into the texture, no font involved) with a colon dot-pair between each
`HH`/`MM`/`SS`/`FF` group. If no valid LTC signal has been decoded in the last second
(source not selected, silent, or not actually carrying LTC), the display falls back
to `--:--:--:--`, rendered as flat segment dashes in the segmented style.

libltc is vendored directly under `plugins/obs-ltc-source/third_party/libltc/`
(LGPL-3.0-or-later, see `COPYING.libltc` there) rather than requiring a system
package, so the build doesn't depend on `libltc-dev` being installed.

## Building

These plugins link against `libobs` and are built as out-of-tree OBS Studio plugin
modules. You need:

- CMake >= 3.16 and a C++17 compiler
- [FreeType2](https://freetype.org/) development headers
- On Linux: fontconfig development headers (`libfontconfig-dev` / `fontconfig-devel`)
- An OBS Studio checkout, built from source (libobs is not published as a
  distro package with headers on most platforms) — see the
  [OBS Studio build instructions](https://github.com/obsproject/obs-studio/wiki)

Then, from this repo:

```sh
cmake -B build \
  -DLIBOBS_INCLUDE_DIR=/path/to/obs-studio/libobs \
  -DLIBOBS_LIB=/path/to/obs-studio/build/libobs/libobs.so
cmake --build build
```

If OBS Studio was installed system-wide via `cmake --install` (which exports a
`libobs` CMake package), you can omit `-DLIBOBS_INCLUDE_DIR` / `-DLIBOBS_LIB` and
`find_package(libobs)` will locate it automatically.

The one command above builds **both** plugins — the root `CMakeLists.txt`
`add_subdirectory()`s every plugin.

### Building on Windows

You don't need the full OBS Studio UI to get `libobs` headers/lib to link
against — build a headless `libobs` only, then build this repo against it.

1. **Install tools** (PowerShell):

   ```powershell
   winget install --id Git.Git -e
   winget install --id Kitware.CMake -e
   winget install --id Microsoft.VisualStudio.2022.Community -e `
       --override "--add Microsoft.VisualStudio.Workload.NativeDesktop --includeRecommended"
   ```

   Fully close and reopen the terminal afterwards so `PATH` picks up the new
   installs — a stale `PATH` in an already-open shell is the most common
   cause of "command not found" here.

2. **Build a headless `libobs`.** Check what OBS Studio version you have
   installed (Help → About) and check out the matching tag if possible, to
   minimize ABI drift risk:

   ```powershell
   git clone --recursive https://github.com/obsproject/obs-studio.git C:\obs-studio
   cd C:\obs-studio
   git checkout <tag, e.g. 32.2.2>
   cmake -B build_x64 -G "Visual Studio 17 2022" -A x64 `
       -DENABLE_FRONTEND=OFF -DENABLE_BROWSER=OFF `
       -DENABLE_SCRIPTING=OFF -DENABLE_PLUGINS=OFF
   cmake --build build_x64 --config RelWithDebInfo
   ```

   Don't pass `-DCMAKE_TOOLCHAIN_FILE=vcpkg` to *this* configure step —
   obs-studio's own CMake auto-bootstraps prebuilt deps on Windows, and a
   vcpkg toolchain file can interfere with that. If `-DENABLE_FRONTEND=OFF`
   is rejected by your checkout, try `-DENABLE_UI=OFF` instead (it's been
   renamed across versions). `cmake --build ... --target obs` will fail with
   `MSB1009` — omit `--target` and just build the default target.

   This produces `C:\obs-studio\build_x64\libobs\RelWithDebInfo\obs.lib`
   (+ `obs.dll`) and `C:\obs-studio\build_x64\config\obsconfig.h`.

3. **Get FreeType via vcpkg** (prefer the static triplet, so you don't have
   to chase and copy transitive runtime DLLs like zlib/libpng/bzip2 next to
   the plugin at install time):

   ```powershell
   git clone https://github.com/microsoft/vcpkg C:\vcpkg
   C:\vcpkg\bootstrap-vcpkg.bat
   C:\vcpkg\vcpkg install freetype:x64-windows-static
   ```

   (`libltc` doesn't need vcpkg — it's vendored as source directly in
   `plugins/obs-ltc-source/third_party/libltc/` and builds as part of this
   repo's own CMake. Fontconfig is Linux-only and is skipped automatically
   on Windows; font family lookup falls back to a couple of hardcoded system
   font paths there, see `src/font-resolver.cpp`.)

4. **Configure and build this repo**, from a `obs-plugins` checkout:

   ```powershell
   cmake -B build -G "Visual Studio 17 2022" -A x64 `
       -DCMAKE_TOOLCHAIN_FILE=C:\vcpkg\scripts\buildsystems\vcpkg.cmake `
       -DVCPKG_TARGET_TRIPLET=x64-windows-static `
       -DLIBOBS_INCLUDE_DIR=C:\obs-studio\libobs `
       -DLIBOBS_LIB=C:\obs-studio\build_x64\libobs\RelWithDebInfo\obs.lib `
       -DLIBOBS_CONFIG_INCLUDE_DIR=C:\obs-studio\build_x64\config
   cmake --build build --config RelWithDebInfo
   ```

   This produces both:

   ```
   build\plugins\obs-datetime-source\RelWithDebInfo\obs-datetime-source.dll
   build\plugins\obs-ltc-source\RelWithDebInfo\obs-ltc-source.dll
   ```

   A `LNK4098` "defaultlib 'LIBCMT' conflicts" warning (dynamic-CRT plugin +
   static-CRT FreeType) is expected and safe to ignore here — the plugin
   only crosses the module boundary through OBS's own API (opaque pointers,
   `bmalloc`/`bfree`), not raw CRT allocations.

## Installing

Copy each built module and its `data/` directory into your OBS Studio
plugins directory.

**Don't want to build from source?** Every [Release](https://github.com/aot93/obs-plugins/releases)
has prebuilt zips for Windows, macOS, and Linux, built automatically by CI
(see `.github/workflows/build.yml`) — each is laid out to match its
platform's target directory structure exactly. Skip straight to the
platform-specific extraction command below; the rest of this section is for
building from source yourself.

**Windows:**

```powershell
Expand-Archive path\to\obs-plugins-windows-<version>.zip -DestinationPath "$env:ProgramData\obs-studio\plugins" -Force
```

**macOS:**

```sh
unzip obs-plugins-macos-<version>.zip -d ~/Library/Application\ Support/obs-studio/plugins/
xattr -cr ~/Library/Application\ Support/obs-studio/plugins/obs-datetime-source.plugin
xattr -cr ~/Library/Application\ Support/obs-studio/plugins/obs-ltc-source.plugin
```

The `xattr -cr` step strips the quarantine flag macOS attaches to anything
downloaded from a browser; without it, Gatekeeper blocks an unsigned,
non-notarized plugin like this one from loading at all ("cannot be opened
because it is from an unidentified developer"), with no mention in OBS's own
log to explain why. **The macOS build is CI-verified to compile and package
correctly, but has not been confirmed to actually load inside a real OBS
Studio app** (no Mac was available to test on) — please file an issue if it
doesn't show up under Sources → +.

**Linux:**

```sh
unzip obs-plugins-linux-<version>.zip -d ~/.config/obs-studio/plugins/
```

Then fully quit and relaunch OBS Studio, and add the source via
**Sources → + → Date/Time Text** or **→ LTC Timecode Source**. If it's
missing from that list, check Help → Log Files → View Current Log for the
plugin's filename or an `obs_module_load` line — no mention at all almost
always means "wrong install path", not a build problem.

### Installing your own build

**Linux:**

```sh
cp build/plugins/obs-datetime-source/obs-datetime-source.so ~/.config/obs-studio/plugins/obs-datetime-source/bin/64bit/
cp -r plugins/obs-datetime-source/data ~/.config/obs-studio/plugins/obs-datetime-source/
```

**Windows — note this is `%ProgramData%`, *not* `%APPDATA%`.** OBS's
third-party plugin scan on Windows uses `%ProgramData%\obs-studio\plugins`;
a plugin dropped under `%APPDATA%\obs-studio\plugins` (the Linux/macOS-style
path) is silently never scanned — no error, no log line.

```powershell
$plugin = "obs-ltc-source"   # or obs-datetime-source
$pluginDir = "$env:ProgramData\obs-studio\plugins\$plugin"
New-Item -ItemType Directory -Force -Path "$pluginDir\bin\64bit" | Out-Null
New-Item -ItemType Directory -Force -Path "$pluginDir\data" | Out-Null
Copy-Item "build\plugins\$plugin\RelWithDebInfo\$plugin.dll" "$pluginDir\bin\64bit\"
Copy-Item -Recurse -Force "plugins\$plugin\data\*" "$pluginDir\data\"
```

**macOS** — built as a `.plugin` bundle (a directory, not a flat file); the
built module has no file extension inside `Contents/MacOS/`:

```sh
plugin="obs-ltc-source"   # or obs-datetime-source
pluginDir="$HOME/Library/Application Support/obs-studio/plugins/$plugin.plugin"
mkdir -p "$pluginDir/Contents/MacOS" "$pluginDir/Contents/Resources"
cp "build/plugins/$plugin/$plugin.so" "$pluginDir/Contents/MacOS/$plugin"   # or .dylib, depending on your CMake/Xcode setup
cp -r "plugins/$plugin/data/"* "$pluginDir/Contents/Resources/"
xattr -cr "$pluginDir"
```

Fully quit and relaunch OBS Studio, then add the source via
**Sources → + → Date/Time Text** or **→ LTC Timecode Source**. If it's
missing from that list, check Help → Log Files → View Current Log for the
plugin's filename or an `obs_module_load` line — no mention at all almost
always means "wrong install path", not a build problem.

## Status

Work in progress — written against the documented libobs source plugin API. Both
plugins build cleanly against a headless `libobs` and (for obs-ltc-source) the core
LTC encode/decode round-trip has been verified standalone, but neither has been
exercised inside a live, fully-installed OBS Studio app in this environment (none is
installed here). Please file an issue (or just try it and report back) if something
doesn't build or behave as expected.

## License

[GPL-2.0-or-later](LICENSE), matching OBS Studio's plugin licensing requirements.
