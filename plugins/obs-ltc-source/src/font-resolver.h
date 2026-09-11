#pragma once

#include <string>

// Resolves a font family name + style flags to a concrete font file path
// that FreeType can load.
//
// Implemented via fontconfig on Linux. On Windows/macOS this currently
// falls back to a small set of well-known system font paths as a stopgap;
// a full DirectWrite / CoreText lookup is not yet implemented (see README).
//
// Returns an empty string if no matching font file could be found.
std::string resolve_font_path(const std::string &face, bool bold, bool italic);
