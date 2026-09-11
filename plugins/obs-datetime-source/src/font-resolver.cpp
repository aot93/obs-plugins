#include "font-resolver.h"

#if defined(HAVE_FONTCONFIG)

#include <fontconfig/fontconfig.h>

std::string resolve_font_path(const std::string &face, bool bold, bool italic)
{
	if (!FcInit())
		return {};

	FcPattern *pattern = FcPatternCreate();
	FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8 *>(face.c_str()));
	FcPatternAddInteger(pattern, FC_WEIGHT, bold ? FC_WEIGHT_BOLD : FC_WEIGHT_REGULAR);
	FcPatternAddInteger(pattern, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);

	FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
	FcDefaultSubstitute(pattern);

	FcResult result;
	FcPattern *match = FcFontMatch(nullptr, pattern, &result);

	std::string path;
	if (match) {
		FcChar8 *file = nullptr;
		if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
			path = reinterpret_cast<const char *>(file);
		FcPatternDestroy(match);
	}

	FcPatternDestroy(pattern);
	return path;
}

#elif defined(_WIN32)

// Stopgap: Windows font family -> file resolution properly requires
// DirectWrite (IDWriteFontCollection). Until that's implemented, fall back
// to a couple of very common system fonts so the plugin is still usable.
std::string resolve_font_path(const std::string &face, bool bold, bool italic)
{
	(void)face;
	if (bold && italic)
		return "C:\\Windows\\Fonts\\arialbi.ttf";
	if (bold)
		return "C:\\Windows\\Fonts\\arialbd.ttf";
	if (italic)
		return "C:\\Windows\\Fonts\\ariali.ttf";
	return "C:\\Windows\\Fonts\\arial.ttf";
}

#elif defined(__APPLE__)

// Stopgap: macOS properly requires CoreText (CTFontCollection) lookup.
// Fall back to a common system font shipped on all recent macOS versions.
std::string resolve_font_path(const std::string &face, bool bold, bool italic)
{
	(void)face;
	(void)bold;
	(void)italic;
	return "/System/Library/Fonts/Supplemental/Arial.ttf";
}

#else

std::string resolve_font_path(const std::string &, bool, bool)
{
	return {};
}

#endif
