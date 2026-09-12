#include "ltc-source.h"
#include "font-resolver.h"
#include "frontend-recording.h"

#include <util/platform.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <tuple>
#include <vector>

// ---------------------------------------------------------------------
// Color helpers (identical packing convention to obs-datetime-source)
// ---------------------------------------------------------------------

static inline void unpack_color(uint32_t val, float &r, float &g, float &b, float &a)
{
	r = (val & 0xFF) / 255.0f;
	g = ((val >> 8) & 0xFF) / 255.0f;
	b = ((val >> 16) & 0xFF) / 255.0f;
	a = ((val >> 24) & 0xFF) / 255.0f;
}

// Composites a solid src color "over" a buffer pixel in place (standard
// Porter-Duff over, straight, non-premultiplied alpha).
static inline void blend_over(std::vector<float> &buf_r, std::vector<float> &buf_g, std::vector<float> &buf_b,
			       std::vector<float> &buf_a, size_t idx, float src_r, float src_g, float src_b,
			       float src_a)
{
	if (src_a <= 0.0f)
		return;
	float dst_a = buf_a[idx];
	float out_a = src_a + dst_a * (1.0f - src_a);
	if (out_a <= 0.0f)
		return;
	buf_r[idx] = (src_r * src_a + buf_r[idx] * dst_a * (1.0f - src_a)) / out_a;
	buf_g[idx] = (src_g * src_a + buf_g[idx] * dst_a * (1.0f - src_a)) / out_a;
	buf_b[idx] = (src_b * src_a + buf_b[idx] * dst_a * (1.0f - src_a)) / out_a;
	buf_a[idx] = out_a;
}

// ---------------------------------------------------------------------
// Audio source attach/detach
//
// We hold a strong reference to the target source for as long as we're
// attached to it (obtained via obs_get_source_by_name). Listening for its
// "remove" signal (fired when the user deletes it, before the source is
// actually torn down) lets us detach and drop that reference so the source
// can still be freed instead of being kept alive indefinitely by us.
// ---------------------------------------------------------------------

static void ltc_source_audio_capture(void *param, obs_source_t *source, const struct audio_data *audio_data,
				      bool muted);
static void ltc_source_target_removed(void *data, calldata_t *cd);

static void detach_audio_source(ltc_source *ls)
{
	if (!ls->audio_source)
		return;

	signal_handler_t *sh = obs_source_get_signal_handler(ls->audio_source);
	signal_handler_disconnect(sh, "remove", ltc_source_target_removed, ls);
	obs_source_remove_audio_capture_callback(ls->audio_source, ltc_source_audio_capture, ls);
	obs_source_release(ls->audio_source);
	ls->audio_source = nullptr;
}

static void ltc_source_target_removed(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(cd);
	detach_audio_source((ltc_source *)data);
}

static void attach_audio_source(ltc_source *ls, const std::string &name)
{
	detach_audio_source(ls);
	if (name.empty())
		return;

	obs_source_t *src = obs_get_source_by_name(name.c_str());
	if (!src) {
		blog(LOG_WARNING, "[obs-ltc-source] audio source '%s' not found", name.c_str());
		return;
	}

	signal_handler_t *sh = obs_source_get_signal_handler(src);
	signal_handler_connect(sh, "remove", ltc_source_target_removed, ls);
	obs_source_add_audio_capture_callback(src, ltc_source_audio_capture, ls);

	ls->audio_source = src; // keep the ref obtained from obs_get_source_by_name
}

// ---------------------------------------------------------------------
// LTC decoding
// ---------------------------------------------------------------------

static void ltc_source_audio_capture(void *param, obs_source_t *source, const struct audio_data *audio_data,
				      bool muted)
{
	UNUSED_PARAMETER(source);
	UNUSED_PARAMETER(muted);
	ltc_source *ls = (ltc_source *)param;

	if (!ls->decoder || !audio_data->data[0] || audio_data->frames == 0)
		return;

	const float *samples = (const float *)audio_data->data[0];
	ltc_decoder_write_float(ls->decoder, const_cast<float *>(samples), audio_data->frames, ls->sample_pos);
	ls->sample_pos += audio_data->frames;

	LTCFrameExt frame;
	SMPTETimecode tc;
	bool got_frame = false;
	while (ltc_decoder_read(ls->decoder, &frame)) {
		ltc_frame_to_time(&tc, &frame.ltc, 0);
		got_frame = true;
	}

	if (got_frame) {
		std::lock_guard<std::mutex> lock(ls->result_mutex);
		ls->last_timecode = tc;
		ls->has_timecode = true;
		ls->last_decode_time_ns = os_gettime_ns();
	}
}

// ---------------------------------------------------------------------
// Font loading (identical approach to obs-datetime-source)
// ---------------------------------------------------------------------

static bool ensure_font(ltc_source *ls)
{
	if (!ls->ft_library)
		return false;

	std::string path = resolve_font_path(ls->font_face, ls->font_bold, ls->font_italic);
	if (path.empty()) {
		if (ls->ft_face)
			return true; // keep using the previously loaded face
		blog(LOG_WARNING, "[obs-ltc-source] could not resolve font '%s'", ls->font_face.c_str());
		return false;
	}

	if (ls->ft_face && ls->loaded_font_path == path && ls->loaded_font_size == ls->font_size)
		return true;

	if (ls->ft_face) {
		FT_Done_Face(ls->ft_face);
		ls->ft_face = nullptr;
	}

	if (FT_New_Face(ls->ft_library, path.c_str(), 0, &ls->ft_face) != 0) {
		blog(LOG_ERROR, "[obs-ltc-source] failed to load font file '%s'", path.c_str());
		ls->ft_face = nullptr;
		return false;
	}

	FT_Set_Pixel_Sizes(ls->ft_face, 0, (FT_UInt)ls->font_size);
	ls->loaded_font_path = path;
	ls->loaded_font_size = ls->font_size;
	return true;
}

// ---------------------------------------------------------------------
// Rasterization: renders `text` into an RGBA texture using FreeType,
// compositing glyphs "over" an optional solid background color.
// ---------------------------------------------------------------------

static void render_text_to_texture(ltc_source *ls, const std::string &text)
{
	if (text.empty() || !ensure_font(ls) || !ls->ft_face) {
		obs_enter_graphics();
		if (ls->texture) {
			gs_texture_destroy(ls->texture);
			ls->texture = nullptr;
		}
		obs_leave_graphics();
		ls->width = 0;
		ls->height = 0;
		return;
	}

	FT_Face face = ls->ft_face;

	// Pass 1: measure the string's bounding box.
	int pen_x = 0;
	int ascent = 0;
	int descent = 0;
	for (unsigned char c : text) {
		if (FT_Load_Char(face, c, FT_LOAD_DEFAULT) != 0)
			continue;
		FT_GlyphSlot g = face->glyph;
		pen_x += (int)(g->advance.x >> 6);
		int top = (int)(g->metrics.horiBearingY >> 6);
		int bottom = top - (int)(g->metrics.height >> 6);
		if (top > ascent)
			ascent = top;
		if (bottom < -descent)
			descent = -bottom;
	}

	const int padding = 6;
	int width = pen_x + padding * 2;
	int height = ascent + descent + padding * 2;
	if (width <= 0 || height <= 0)
		return;

	float text_r, text_g, text_b, text_a;
	unpack_color(ls->text_color, text_r, text_g, text_b, text_a);
	float bg_r, bg_g, bg_b, bg_a;
	unpack_color(ls->background_color, bg_r, bg_g, bg_b, bg_a);
	if (!ls->background_enabled)
		bg_a = 0.0f;

	std::vector<float> buf_r((size_t)width * height, bg_r);
	std::vector<float> buf_g((size_t)width * height, bg_g);
	std::vector<float> buf_b((size_t)width * height, bg_b);
	std::vector<float> buf_a((size_t)width * height, bg_a);

	// Pass 2: rasterize each glyph and composite it "over" the buffer.
	int pen = padding;
	int baseline = padding + ascent;
	for (unsigned char c : text) {
		if (FT_Load_Char(face, c, FT_LOAD_RENDER) != 0)
			continue;
		FT_GlyphSlot g = face->glyph;
		FT_Bitmap &bmp = g->bitmap;

		int origin_x = pen + g->bitmap_left;
		int origin_y = baseline - g->bitmap_top;

		for (unsigned int row = 0; row < bmp.rows; row++) {
			int y = origin_y + (int)row;
			if (y < 0 || y >= height)
				continue;
			for (unsigned int col = 0; col < bmp.width; col++) {
				int x = origin_x + (int)col;
				if (x < 0 || x >= width)
					continue;

				float glyph_a = bmp.buffer[row * bmp.pitch + col] / 255.0f;
				float src_a = glyph_a * text_a;
				size_t idx = (size_t)y * width + x;
				blend_over(buf_r, buf_g, buf_b, buf_a, idx, text_r, text_g, text_b, src_a);
			}
		}

		pen += (int)(g->advance.x >> 6);
	}

	std::vector<uint8_t> pixels((size_t)width * height * 4);
	for (int i = 0; i < width * height; i++) {
		pixels[(size_t)i * 4 + 0] = (uint8_t)(buf_r[i] * 255.0f + 0.5f);
		pixels[(size_t)i * 4 + 1] = (uint8_t)(buf_g[i] * 255.0f + 0.5f);
		pixels[(size_t)i * 4 + 2] = (uint8_t)(buf_b[i] * 255.0f + 0.5f);
		pixels[(size_t)i * 4 + 3] = (uint8_t)(buf_a[i] * 255.0f + 0.5f);
	}

	const uint8_t *data_ptr = pixels.data();

	obs_enter_graphics();
	if (ls->texture) {
		gs_texture_destroy(ls->texture);
		ls->texture = nullptr;
	}
	ls->texture = gs_texture_create((uint32_t)width, (uint32_t)height, GS_RGBA, 1, &data_ptr, 0);
	obs_leave_graphics();

	ls->width = (uint32_t)width;
	ls->height = (uint32_t)height;
}

// ---------------------------------------------------------------------
// Rasterization: classic "8-segment" (7 segments + colon) LED/LCD clock
// look. Each digit is drawn as a set of solid rectangles rather than a
// font glyph; segments not lit for the current digit are optionally drawn
// in a dim "ghost" color, matching an unpowered real segment display.
// ---------------------------------------------------------------------

// Segment bit order: a=top, b=top-right, c=bottom-right, d=bottom,
// e=bottom-left, f=top-left, g=middle.
static const uint8_t seven_seg_digits[10] = {
	0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};
// Middle bar only -- used to draw non-digit characters (e.g. the '-' in the
// "--:--:--:--" no-signal placeholder) with the same segmented look.
static const uint8_t seven_seg_dash = 0x40;

struct seg_rect {
	float x0, y0, x1, y1; // normalized [0,1] within a single digit cell
};

// Lays out the 7 segment rectangles for a given segment thickness (as a
// fraction of the digit cell's width), with a small gap between segments so
// they read as distinct bars rather than a solid block.
static void seven_seg_rects(float thickness, seg_rect out[7])
{
	float t = std::clamp(thickness, 0.05f, 0.45f);
	float gp = std::clamp(t * 0.18f, 0.01f, 0.06f);

	out[0] = {gp, 0.0f, 1.0f - gp, t};                           // a: top
	out[1] = {1.0f - t, gp, 1.0f, 0.5f - gp * 0.5f};              // b: top-right
	out[2] = {1.0f - t, 0.5f + gp * 0.5f, 1.0f, 1.0f - gp};       // c: bottom-right
	out[3] = {gp, 1.0f - t, 1.0f - gp, 1.0f};                     // d: bottom
	out[4] = {0.0f, 0.5f + gp * 0.5f, t, 1.0f - gp};              // e: bottom-left
	out[5] = {0.0f, gp, t, 0.5f - gp * 0.5f};                     // f: top-left
	out[6] = {gp, 0.5f - t * 0.5f, 1.0f - gp, 0.5f + t * 0.5f};   // g: middle
}

// Fills a normalized [0,1] rect, scaled to a cell of size (cell_w, cell_h)
// positioned at (cell_x, cell_y) in canvas pixel space, compositing "over"
// the existing buffer contents.
static void fill_cell_rect(std::vector<float> &buf_r, std::vector<float> &buf_g, std::vector<float> &buf_b,
			    std::vector<float> &buf_a, int canvas_w, int canvas_h, int cell_x, int cell_y,
			    int cell_w, int cell_h, const seg_rect &rect, float r, float g, float b, float a)
{
	if (a <= 0.0f)
		return;

	int x0 = std::max(0, cell_x + (int)(rect.x0 * cell_w + 0.5f));
	int x1 = std::min(canvas_w, cell_x + (int)(rect.x1 * cell_w + 0.5f));
	int y0 = std::max(0, cell_y + (int)(rect.y0 * cell_h + 0.5f));
	int y1 = std::min(canvas_h, cell_y + (int)(rect.y1 * cell_h + 0.5f));

	for (int y = y0; y < y1; y++) {
		for (int x = x0; x < x1; x++)
			blend_over(buf_r, buf_g, buf_b, buf_a, (size_t)y * canvas_w + x, r, g, b, a);
	}
}

static void render_segmented_to_texture(ltc_source *ls, const std::string &text)
{
	if (text.empty()) {
		obs_enter_graphics();
		if (ls->texture) {
			gs_texture_destroy(ls->texture);
			ls->texture = nullptr;
		}
		obs_leave_graphics();
		ls->width = 0;
		ls->height = 0;
		return;
	}

	int digit_h = ls->digit_height > 0 ? ls->digit_height : 96;
	int digit_w = std::max(1, (int)(digit_h * 0.55f + 0.5f));
	int colon_w = std::max(1, (int)(digit_h * 0.32f + 0.5f));
	int spacing = (int)(digit_h * 0.16f + 0.5f);
	int padding = (int)(digit_h * 0.14f + 0.5f);

	// Pass 1: sum each character's cell width to get the overall canvas size.
	int content_w = 0;
	for (size_t i = 0; i < text.size(); i++) {
		content_w += (text[i] == ':') ? colon_w : digit_w;
		if (i + 1 < text.size())
			content_w += spacing;
	}

	int width = content_w + padding * 2;
	int height = digit_h + padding * 2;
	if (width <= 0 || height <= 0)
		return;

	float on_r, on_g, on_b, on_a;
	unpack_color(ls->segment_on_color, on_r, on_g, on_b, on_a);
	float off_r, off_g, off_b, off_a;
	unpack_color(ls->segment_off_color, off_r, off_g, off_b, off_a);
	if (!ls->segment_off_enabled)
		off_a = 0.0f;
	float bg_r, bg_g, bg_b, bg_a;
	unpack_color(ls->background_color, bg_r, bg_g, bg_b, bg_a);
	if (!ls->background_enabled)
		bg_a = 0.0f;

	std::vector<float> buf_r((size_t)width * height, bg_r);
	std::vector<float> buf_g((size_t)width * height, bg_g);
	std::vector<float> buf_b((size_t)width * height, bg_b);
	std::vector<float> buf_a((size_t)width * height, bg_a);

	seg_rect rects[7];
	seven_seg_rects(ls->segment_thickness, rects);
	const seg_rect whole_cell = {0.0f, 0.0f, 1.0f, 1.0f};

	// Pass 2: draw each character's cell.
	int pen_x = padding;
	for (size_t i = 0; i < text.size(); i++) {
		char c = text[i];

		if (c == ':') {
			// A fixed separator: always lit, not digit-dependent.
			int dot = std::max(1, (int)(std::min(colon_w, digit_h) * 0.30f));
			int cx = pen_x + colon_w / 2 - dot / 2;
			int cy1 = padding + (int)(digit_h * 0.32f) - dot / 2;
			int cy2 = padding + (int)(digit_h * 0.68f) - dot / 2;
			fill_cell_rect(buf_r, buf_g, buf_b, buf_a, width, height, cx, cy1, dot, dot, whole_cell, on_r,
				       on_g, on_b, on_a);
			fill_cell_rect(buf_r, buf_g, buf_b, buf_a, width, height, cx, cy2, dot, dot, whole_cell, on_r,
				       on_g, on_b, on_a);
			pen_x += colon_w;
		} else {
			uint8_t mask = (c >= '0' && c <= '9') ? seven_seg_digits[c - '0'] : seven_seg_dash;

			for (int seg = 0; seg < 7; seg++) {
				bool lit = (mask >> seg) & 1;
				if (lit)
					fill_cell_rect(buf_r, buf_g, buf_b, buf_a, width, height, pen_x, padding,
						       digit_w, digit_h, rects[seg], on_r, on_g, on_b, on_a);
				else
					fill_cell_rect(buf_r, buf_g, buf_b, buf_a, width, height, pen_x, padding,
						       digit_w, digit_h, rects[seg], off_r, off_g, off_b, off_a);
			}
			pen_x += digit_w;
		}

		if (i + 1 < text.size())
			pen_x += spacing;
	}

	std::vector<uint8_t> pixels((size_t)width * height * 4);
	for (int i = 0; i < width * height; i++) {
		pixels[(size_t)i * 4 + 0] = (uint8_t)(buf_r[i] * 255.0f + 0.5f);
		pixels[(size_t)i * 4 + 1] = (uint8_t)(buf_g[i] * 255.0f + 0.5f);
		pixels[(size_t)i * 4 + 2] = (uint8_t)(buf_b[i] * 255.0f + 0.5f);
		pixels[(size_t)i * 4 + 3] = (uint8_t)(buf_a[i] * 255.0f + 0.5f);
	}

	const uint8_t *data_ptr = pixels.data();

	obs_enter_graphics();
	if (ls->texture) {
		gs_texture_destroy(ls->texture);
		ls->texture = nullptr;
	}
	ls->texture = gs_texture_create((uint32_t)width, (uint32_t)height, GS_RGBA, 1, &data_ptr, 0);
	obs_leave_graphics();

	ls->width = (uint32_t)width;
	ls->height = (uint32_t)height;
}

// ---------------------------------------------------------------------
// Ignore ranges: user-specified timecode windows (e.g. pre-show slate, test
// sequences) within which the auto-record trigger must never fire, even
// though the incoming LTC is advancing normally.
//
// Text format: comma-separated "HH:MM:SS:FF-HH:MM:SS:FF" pairs, e.g.
// "22:00:00:00-23:00:00:00, 23:30:00:00-23:45:00:00". Both ends inclusive.
// ---------------------------------------------------------------------

static std::string trim(const std::string &s)
{
	size_t a = s.find_first_not_of(" \t\r\n");
	if (a == std::string::npos)
		return "";
	size_t b = s.find_last_not_of(" \t\r\n");
	return s.substr(a, b - a + 1);
}

static bool parse_timecode_token(const std::string &token, int out[4])
{
	int h, m, s, f;
	if (sscanf(token.c_str(), "%d:%d:%d:%d", &h, &m, &s, &f) != 4)
		return false;
	out[0] = h;
	out[1] = m;
	out[2] = s;
	out[3] = f;
	return true;
}

static std::vector<ltc_ignore_range> parse_ignore_ranges(const std::string &text)
{
	std::vector<ltc_ignore_range> ranges;

	size_t pos = 0;
	while (pos <= text.size()) {
		size_t comma = text.find(',', pos);
		std::string part = trim(text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
		pos = (comma == std::string::npos) ? text.size() + 1 : comma + 1;
		if (part.empty())
			continue;

		size_t dash = part.find('-');
		ltc_ignore_range r;
		if (dash == std::string::npos || !parse_timecode_token(trim(part.substr(0, dash)), r.start) ||
		    !parse_timecode_token(trim(part.substr(dash + 1)), r.end)) {
			blog(LOG_WARNING, "[obs-ltc-source] ignoring malformed ignore-range entry: '%s'", part.c_str());
			continue;
		}
		ranges.push_back(r);
	}

	return ranges;
}

static bool tc_in_ignore_range(const SMPTETimecode &tc, const ltc_ignore_range &r)
{
	auto key = std::tie(tc.hours, tc.mins, tc.secs, tc.frame);
	auto start = std::tie(r.start[0], r.start[1], r.start[2], r.start[3]);
	auto end = std::tie(r.end[0], r.end[1], r.end[2], r.end[3]);
	return !(key < start) && !(end < key);
}

static bool is_ignored_timecode(const ltc_source *ls, const SMPTETimecode &tc)
{
	for (const auto &r : ls->ignore_ranges) {
		if (tc_in_ignore_range(tc, r))
			return true;
	}
	return false;
}

// ---------------------------------------------------------------------
// Automatic recording start/stop, driven by whether the incoming timecode
// is actively advancing.
//
// "Running" latches only after `record_start_min_frames` consecutive
// decoded frames each differ from the previous one -- this guards against a
// handful of garbled/spurious decodes from a noisy signal falsely
// triggering a start. Once latched, a stop only fires after the signal has
// been either lost entirely or frozen on the same value for
// `record_stop_wait_seconds` -- this guards against a momentary signal
// glitch or dropped LTC frame prematurely stopping the recording.
//
// While the current timecode falls within a user-configured ignore range,
// neither the advance streak nor the "stopped since" timer are allowed to
// keep recording going: it's treated exactly like a lost/frozen signal, so
// a pre-show slate or test sequence can never trigger (or extend) a
// recording, and any recording already running when an ignore range is
// entered winds down on the normal stop-wait schedule.
// ---------------------------------------------------------------------

static void update_auto_record(ltc_source *ls, bool stale, bool has_tc, const SMPTETimecode &tc, uint64_t decode_time)
{
	bool new_decode = has_tc && decode_time != ls->last_processed_decode_time_ns;
	if (new_decode) {
		ls->last_processed_decode_time_ns = decode_time;

		bool advanced = ls->prev_decoded_valid &&
				(tc.hours != ls->prev_decoded_tc.hours || tc.mins != ls->prev_decoded_tc.mins ||
				 tc.secs != ls->prev_decoded_tc.secs || tc.frame != ls->prev_decoded_tc.frame);
		bool ignored = is_ignored_timecode(ls, tc);
		ls->prev_decoded_tc = tc;
		ls->prev_decoded_valid = true;

		if (advanced && !ignored) {
			ls->advance_streak++;
			ls->stopped_since_ns = 0;
			if (!ls->tc_running && ls->auto_record_enabled &&
			    ls->advance_streak >= std::max(1, ls->record_start_min_frames)) {
				ls->tc_running = true;
				if (!ltc_frontend_recording_active())
					ltc_frontend_recording_start();
			}
		} else {
			// Same value as the previous decode (e.g. a paused deck still
			// transmitting LTC), or inside a user-configured ignore range.
			ls->advance_streak = 0;
		}
	}

	bool currently_ignored = ls->prev_decoded_valid && is_ignored_timecode(ls, ls->prev_decoded_tc);
	bool not_advancing = stale || ls->advance_streak == 0 || currently_ignored;
	if (!not_advancing) {
		ls->stopped_since_ns = 0;
	} else if (ls->tc_running) {
		uint64_t now = os_gettime_ns();
		if (ls->stopped_since_ns == 0) {
			ls->stopped_since_ns = now;
		} else {
			uint64_t wait_ns = (uint64_t)(std::max(0.0f, ls->record_stop_wait_seconds) * 1000000000.0);
			if (now - ls->stopped_since_ns >= wait_ns) {
				ls->tc_running = false;
				ls->stopped_since_ns = 0;
				if (ls->auto_record_enabled && ltc_frontend_recording_active())
					ltc_frontend_recording_stop();
			}
		}
	}
}

// ---------------------------------------------------------------------
// obs_source_info callbacks
// ---------------------------------------------------------------------

static const char *ltc_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("LTCSource.Name");
}

static bool enum_audio_source_cb(void *param, obs_source_t *src)
{
	obs_property_t *list = (obs_property_t *)param;
	uint32_t flags = obs_source_get_output_flags(src);
	if (!(flags & OBS_SOURCE_AUDIO))
		return true;

	const char *name = obs_source_get_name(src);
	if (name && *name)
		obs_property_list_add_string(list, name, name);
	return true;
}

static void ltc_source_update(void *data, obs_data_t *settings)
{
	ltc_source *ls = (ltc_source *)data;

	std::string new_audio_source = obs_data_get_string(settings, "audio_source");
	if (new_audio_source != ls->audio_source_name) {
		ls->audio_source_name = new_audio_source;
		attach_audio_source(ls, ls->audio_source_name);
	}

	ls->display_style = (ltc_display_style)obs_data_get_int(settings, "style");

	ls->text_color = (uint32_t)obs_data_get_int(settings, "color");
	ls->background_enabled = obs_data_get_bool(settings, "background_enabled");
	ls->background_color = (uint32_t)obs_data_get_int(settings, "background_color");

	ls->segment_on_color = (uint32_t)obs_data_get_int(settings, "segment_on_color");
	ls->segment_off_enabled = obs_data_get_bool(settings, "segment_off_enabled");
	ls->segment_off_color = (uint32_t)obs_data_get_int(settings, "segment_off_color");
	ls->digit_height = (int)obs_data_get_int(settings, "digit_height");
	if (ls->digit_height <= 0)
		ls->digit_height = 96;
	ls->segment_thickness = (float)obs_data_get_double(settings, "segment_thickness");
	if (ls->segment_thickness <= 0.0f || ls->segment_thickness > 0.5f)
		ls->segment_thickness = 0.20f;

	ls->auto_record_enabled = obs_data_get_bool(settings, "auto_record_enabled");
	ls->record_start_min_frames = (int)obs_data_get_int(settings, "record_start_min_frames");
	if (ls->record_start_min_frames < 1)
		ls->record_start_min_frames = 1;
	ls->record_stop_wait_seconds = (float)obs_data_get_double(settings, "record_stop_wait_seconds");
	if (ls->record_stop_wait_seconds < 0.0f)
		ls->record_stop_wait_seconds = 0.0f;
	ls->ignore_ranges_text = obs_data_get_string(settings, "ignore_ranges");
	ls->ignore_ranges = parse_ignore_ranges(ls->ignore_ranges_text);

	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	if (font_obj) {
		ls->font_face = obs_data_get_string(font_obj, "face");
		ls->font_size = (int)obs_data_get_int(font_obj, "size");
		long long flags = obs_data_get_int(font_obj, "flags");
		ls->font_bold = (flags & OBS_FONT_BOLD) != 0;
		ls->font_italic = (flags & OBS_FONT_ITALIC) != 0;
		obs_data_release(font_obj);
	}
	if (ls->font_size <= 0)
		ls->font_size = 64;
	if (ls->font_face.empty())
		ls->font_face = "Monospace";

	// Force a re-render on the next tick even if the displayed text hasn't
	// changed (e.g. a font/color change).
	ls->last_rendered_text.clear();
}

static void ltc_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "audio_source", "");
	obs_data_set_default_int(settings, "style", (int)ltc_display_style::SEGMENTED);

	obs_data_set_default_int(settings, "color", (int)0xFFFFFFFF);
	obs_data_set_default_bool(settings, "background_enabled", false);
	obs_data_set_default_int(settings, "background_color", (int)0xFF000000);

	obs_data_set_default_int(settings, "segment_on_color", (int)0xFF0000FF); // opaque red
	obs_data_set_default_bool(settings, "segment_off_enabled", true);
	obs_data_set_default_int(settings, "segment_off_color", (int)0x40000030); // dim "ghost" red
	obs_data_set_default_int(settings, "digit_height", 96);
	obs_data_set_default_double(settings, "segment_thickness", 0.20);

	obs_data_set_default_bool(settings, "auto_record_enabled", false);
	obs_data_set_default_int(settings, "record_start_min_frames", 5);
	obs_data_set_default_double(settings, "record_stop_wait_seconds", 2.0);
	obs_data_set_default_string(settings, "ignore_ranges", "");

	obs_data_t *font_obj = obs_data_create();
	obs_data_set_default_string(font_obj, "face", "Monospace");
	obs_data_set_default_int(font_obj, "size", 96);
	obs_data_set_default_int(font_obj, "flags", 0);
	obs_data_set_default_obj(settings, "font", font_obj);
	obs_data_release(font_obj);
}

static bool style_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	bool segmented = (int)obs_data_get_int(settings, "style") == (int)ltc_display_style::SEGMENTED;

	obs_property_set_visible(obs_properties_get(props, "font"), !segmented);
	obs_property_set_visible(obs_properties_get(props, "color"), !segmented);

	obs_property_set_visible(obs_properties_get(props, "segment_on_color"), segmented);
	obs_property_set_visible(obs_properties_get(props, "segment_off_enabled"), segmented);
	obs_property_set_visible(obs_properties_get(props, "segment_off_color"), segmented);
	obs_property_set_visible(obs_properties_get(props, "digit_height"), segmented);
	obs_property_set_visible(obs_properties_get(props, "segment_thickness"), segmented);
	return true;
}

static bool auto_record_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	bool enabled = obs_data_get_bool(settings, "auto_record_enabled");
	obs_property_set_visible(obs_properties_get(props, "record_start_min_frames"), enabled);
	obs_property_set_visible(obs_properties_get(props, "record_stop_wait_seconds"), enabled);
	obs_property_set_visible(obs_properties_get(props, "ignore_ranges"), enabled);
	return true;
}

static obs_properties_t *ltc_source_get_properties(void *data)
{
	ltc_source *ls = (ltc_source *)data;

	obs_properties_t *props = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(props, "audio_source", obs_module_text("AudioSource"),
							OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(list, obs_module_text("AudioSource.None"), "");
	obs_enum_sources(enum_audio_source_cb, list);

	obs_property_t *style_list = obs_properties_add_list(props, "style", obs_module_text("Style"),
							      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(style_list, obs_module_text("Style.Segmented"), (int)ltc_display_style::SEGMENTED);
	obs_property_list_add_int(style_list, obs_module_text("Style.PlainText"), (int)ltc_display_style::PLAIN_TEXT);
	obs_property_set_modified_callback(style_list, style_modified);

	obs_properties_add_font(props, "font", obs_module_text("Font"));
	obs_properties_add_color_alpha(props, "color", obs_module_text("Color"));

	obs_properties_add_color_alpha(props, "segment_on_color", obs_module_text("SegmentOnColor"));
	obs_properties_add_bool(props, "segment_off_enabled", obs_module_text("SegmentOffEnabled"));
	obs_properties_add_color_alpha(props, "segment_off_color", obs_module_text("SegmentOffColor"));
	obs_properties_add_int_slider(props, "digit_height", obs_module_text("DigitHeight"), 24, 400, 2);
	obs_properties_add_float_slider(props, "segment_thickness", obs_module_text("SegmentThickness"), 0.08, 0.35,
					 0.01);

	obs_properties_add_bool(props, "background_enabled", obs_module_text("BackgroundEnabled"));
	obs_properties_add_color_alpha(props, "background_color", obs_module_text("BackgroundColor"));

	obs_property_t *auto_record = obs_properties_add_bool(props, "auto_record_enabled",
								obs_module_text("AutoRecordEnabled"));
	obs_property_set_modified_callback(auto_record, auto_record_modified);
	obs_properties_add_int_slider(props, "record_start_min_frames", obs_module_text("RecordStartMinFrames"), 1, 60,
				       1);
	obs_properties_add_float_slider(props, "record_stop_wait_seconds", obs_module_text("RecordStopWaitSeconds"),
					 0.0, 30.0, 0.5);
	obs_property_t *ignore_ranges = obs_properties_add_text(props, "ignore_ranges",
								  obs_module_text("IgnoreRanges"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(ignore_ranges, obs_module_text("IgnoreRanges.Description"));

	if (ls) {
		bool segmented = ls->display_style == ltc_display_style::SEGMENTED;
		obs_property_set_visible(obs_properties_get(props, "font"), !segmented);
		obs_property_set_visible(obs_properties_get(props, "color"), !segmented);
		obs_property_set_visible(obs_properties_get(props, "segment_on_color"), segmented);
		obs_property_set_visible(obs_properties_get(props, "segment_off_enabled"), segmented);
		obs_property_set_visible(obs_properties_get(props, "segment_off_color"), segmented);
		obs_property_set_visible(obs_properties_get(props, "digit_height"), segmented);
		obs_property_set_visible(obs_properties_get(props, "segment_thickness"), segmented);

		obs_property_set_visible(obs_properties_get(props, "record_start_min_frames"), ls->auto_record_enabled);
		obs_property_set_visible(obs_properties_get(props, "record_stop_wait_seconds"), ls->auto_record_enabled);
		obs_property_set_visible(obs_properties_get(props, "ignore_ranges"), ls->auto_record_enabled);
	}

	return props;
}

static void *ltc_source_create(obs_data_t *settings, obs_source_t *source)
{
	ltc_source *ls = new ltc_source();
	ls->source = source;

	if (FT_Init_FreeType(&ls->ft_library) != 0) {
		blog(LOG_ERROR, "[obs-ltc-source] failed to initialize FreeType");
		ls->ft_library = nullptr;
	}

	obs_audio_info ai;
	uint32_t sample_rate = obs_get_audio_info(&ai) ? ai.samples_per_sec : 48000;
	// Seed value only: libltc tracks the actual bit-rate/speed dynamically
	// once it locks onto a signal. 25fps is a reasonable generic default.
	int apv = (int)(sample_rate / 25.0 + 0.5);
	ls->decoder = ltc_decoder_create(apv, 32);
	if (!ls->decoder)
		blog(LOG_ERROR, "[obs-ltc-source] failed to create LTC decoder");

	ltc_source_update(ls, settings);
	return ls;
}

static void ltc_source_destroy(void *data)
{
	ltc_source *ls = (ltc_source *)data;

	detach_audio_source(ls);

	if (ls->decoder)
		ltc_decoder_free(ls->decoder);

	if (ls->texture) {
		obs_enter_graphics();
		gs_texture_destroy(ls->texture);
		obs_leave_graphics();
	}
	if (ls->ft_face)
		FT_Done_Face(ls->ft_face);
	if (ls->ft_library)
		FT_Done_FreeType(ls->ft_library);

	delete ls;
}

static void ltc_source_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	ltc_source *ls = (ltc_source *)data;

	SMPTETimecode tc;
	bool has_tc;
	uint64_t decode_time;
	{
		std::lock_guard<std::mutex> lock(ls->result_mutex);
		tc = ls->last_timecode;
		has_tc = ls->has_timecode;
		decode_time = ls->last_decode_time_ns;
	}

	// Treat the signal as lost if we haven't decoded a fresh frame in over
	// a second, so a disconnected/silent source doesn't freeze on stale text.
	const uint64_t timeout_ns = 1000000000ULL;
	bool stale = !has_tc || (os_gettime_ns() - decode_time) > timeout_ns;

	update_auto_record(ls, stale, has_tc, tc, decode_time);

	std::string text;
	if (stale) {
		text = "--:--:--:--";
	} else {
		char buf[32];
		snprintf(buf, sizeof(buf), "%02u:%02u:%02u:%02u", (unsigned)tc.hours, (unsigned)tc.mins,
			 (unsigned)tc.secs, (unsigned)tc.frame);
		text = buf;
	}

	if (text == ls->last_rendered_text)
		return;

	if (ls->display_style == ltc_display_style::SEGMENTED)
		render_segmented_to_texture(ls, text);
	else
		render_text_to_texture(ls, text);
	ls->last_rendered_text = text;
}

static void ltc_source_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	ltc_source *ls = (ltc_source *)data;
	if (!ls->texture)
		return;

	gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(default_effect, "image");
	gs_effect_set_texture(image, ls->texture);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	while (gs_effect_loop(default_effect, "Draw"))
		gs_draw_sprite(ls->texture, 0, ls->width, ls->height);

	gs_blend_state_pop();
}

static uint32_t ltc_source_get_width(void *data)
{
	return ((ltc_source *)data)->width;
}

static uint32_t ltc_source_get_height(void *data)
{
	return ((ltc_source *)data)->height;
}

// ---------------------------------------------------------------------
// Registration
//
// Assigned via a static initializer rather than a C-style designated
// initializer, since obs_source_info's field order isn't part of libobs'
// stable ABI/API contract and C++ designated initializers require the
// initializers to appear in declaration order.
// ---------------------------------------------------------------------

struct obs_source_info ltc_source_info = {};

namespace {
struct ltc_source_info_registrar {
	ltc_source_info_registrar()
	{
		ltc_source_info.id = "ltc_timecode_source";
		ltc_source_info.type = OBS_SOURCE_TYPE_INPUT;
		ltc_source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
		ltc_source_info.get_name = ltc_source_get_name;
		ltc_source_info.create = ltc_source_create;
		ltc_source_info.destroy = ltc_source_destroy;
		ltc_source_info.update = ltc_source_update;
		ltc_source_info.get_defaults = ltc_source_get_defaults;
		ltc_source_info.get_properties = ltc_source_get_properties;
		ltc_source_info.video_tick = ltc_source_video_tick;
		ltc_source_info.video_render = ltc_source_video_render;
		ltc_source_info.get_width = ltc_source_get_width;
		ltc_source_info.get_height = ltc_source_get_height;
		ltc_source_info.icon_type = OBS_ICON_TYPE_TEXT;
	}
} g_ltc_source_info_registrar;
} // namespace
