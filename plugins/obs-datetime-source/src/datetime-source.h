#pragma once

#include <obs-module.h>
#include <graphics/graphics.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include <ctime>
#include <string>

enum class datetime_mode {
	DATE_ONLY = 0,
	TIME_ONLY = 1,
	DATE_AND_TIME = 2,
	CUSTOM = 3,
};

struct datetime_source {
	obs_source_t *source = nullptr;

	// settings
	datetime_mode mode = datetime_mode::DATE_AND_TIME;
	std::string custom_format;
	std::string font_face;
	int font_size = 64;
	bool font_bold = false;
	bool font_italic = false;
	uint32_t text_color = 0xFFFFFFFF; // packed R,G,B,A (LSB->MSB), matches libobs vec4_from_rgba
	bool background_enabled = false;
	uint32_t background_color = 0xFF000000;

	// render state
	FT_Library ft_library = nullptr;
	FT_Face ft_face = nullptr;
	std::string loaded_font_path;
	int loaded_font_size = 0;

	gs_texture_t *texture = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;

	std::string last_rendered_text;
	time_t last_rendered_time = 0;
};

extern struct obs_source_info datetime_source_info;
