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

## Installing

Copy the built module and its `data/` directory into your OBS Studio plugins
directory, e.g. on Linux:

```sh
cp build/plugins/obs-datetime-source/obs-datetime-source.so ~/.config/obs-studio/plugins/obs-datetime-source/bin/64bit/
cp -r plugins/obs-datetime-source/data ~/.config/obs-studio/plugins/obs-datetime-source/
```

Restart OBS Studio, then add the source via **Sources → + → Date/Time Text** (or
**→ LTC Timecode Source** for `obs-ltc-source`, substituting its own module/data
paths above).

## Status

Work in progress — written against the documented libobs source plugin API. Both
plugins build cleanly against a headless `libobs` and (for obs-ltc-source) the core
LTC encode/decode round-trip has been verified standalone, but neither has been
exercised inside a live, fully-installed OBS Studio app in this environment (none is
installed here). Please file an issue (or just try it and report back) if something
doesn't build or behave as expected.

## License

[GPL-2.0-or-later](LICENSE), matching OBS Studio's plugin licensing requirements.
