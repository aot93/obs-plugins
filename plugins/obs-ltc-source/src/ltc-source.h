#pragma once

#include <obs-module.h>
#include <graphics/graphics.h>
#include <ft2build.h>
#include FT_FREETYPE_H

extern "C" {
#include <ltc.h>
}

#include <mutex>
#include <string>

enum class ltc_display_style {
	PLAIN_TEXT = 0,
	SEGMENTED = 1,
};

struct ltc_source {
	obs_source_t *source = nullptr;

	// settings
	std::string audio_source_name;
	ltc_display_style display_style = ltc_display_style::SEGMENTED;

	// plain-text style (FreeType)
	std::string font_face;
	int font_size = 64;
	bool font_bold = false;
	bool font_italic = false;
	uint32_t text_color = 0xFFFFFFFF; // packed R,G,B,A (LSB->MSB), matches libobs vec4_from_rgba

	// segmented ("classic" LED clock) style
	int digit_height = 96;
	float segment_thickness = 0.20f; // fraction of a digit cell's width
	uint32_t segment_on_color = 0xFF0000FF;
	bool segment_off_enabled = true;
	uint32_t segment_off_color = 0x40000030;

	// shared between both styles
	bool background_enabled = false;
	uint32_t background_color = 0xFF000000;

	// audio source we're attached to (strong ref held while attached; see
	// attach_audio_source/detach_audio_source in ltc-source.cpp)
	obs_source_t *audio_source = nullptr;

	// LTC decoding (decoder is only ever touched from the audio thread that
	// delivers samples via the capture callback)
	LTCDecoder *decoder = nullptr;
	int64_t sample_pos = 0;

	// latest decoded result, shared between the audio-capture callback
	// (writer) and video_tick (reader)
	std::mutex result_mutex;
	bool has_timecode = false;
	SMPTETimecode last_timecode{};
	uint64_t last_decode_time_ns = 0;

	// text rendering state (FreeType)
	FT_Library ft_library = nullptr;
	FT_Face ft_face = nullptr;
	std::string loaded_font_path;
	int loaded_font_size = 0;

	gs_texture_t *texture = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;

	std::string last_rendered_text;
};

extern struct obs_source_info ltc_source_info;
