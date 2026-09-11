#include "datetime-source.h"
#include "font-resolver.h"

#include <cstdint>
#include <cstring>
#include <vector>

// ---------------------------------------------------------------------
// Formatting
// ---------------------------------------------------------------------

static std::string default_format_for_mode(datetime_mode mode)
{
	switch (mode) {
	case datetime_mode::DATE_ONLY:
		return "%d %b %Y";
	case datetime_mode::TIME_ONLY:
		return "%H:%M:%S";
	case datetime_mode::DATE_AND_TIME:
	case datetime_mode::CUSTOM:
	default:
		return "%d %b %Y  %H:%M:%S";
	}
}

// NOTE: strftime() output is treated byte-for-byte as Latin-1/ASCII glyph
// indices below. This covers the default "C"/English locale used by the
// bundled format strings; non-ASCII locale output (e.g. multi-byte UTF-8
// month names) is not decoded and may render incorrectly.
static std::string current_text(const datetime_source *ds, time_t now)
{
	std::string fmt = (ds->mode == datetime_mode::CUSTOM) ? ds->custom_format : default_format_for_mode(ds->mode);
	if (fmt.empty())
		fmt = default_format_for_mode(datetime_mode::DATE_AND_TIME);

	struct tm local_tm;
#if defined(_WIN32)
	localtime_s(&local_tm, &now);
#else
	localtime_r(&now, &local_tm);
#endif

	char buf[256];
	size_t len = strftime(buf, sizeof(buf), fmt.c_str(), &local_tm);
	return len ? std::string(buf, len) : std::string();
}

// ---------------------------------------------------------------------
// Color helpers
// ---------------------------------------------------------------------

// Matches libobs' vec4_from_rgba packing (R in the low byte, A in the high
// byte), so the OBS color picker and our rendering agree on what a given
// packed value looks like.
static inline void unpack_color(uint32_t val, float &r, float &g, float &b, float &a)
{
	r = (val & 0xFF) / 255.0f;
	g = ((val >> 8) & 0xFF) / 255.0f;
	b = ((val >> 16) & 0xFF) / 255.0f;
	a = ((val >> 24) & 0xFF) / 255.0f;
}

// ---------------------------------------------------------------------
// Font loading
// ---------------------------------------------------------------------

static bool ensure_font(datetime_source *ds)
{
	if (!ds->ft_library)
		return false;

	std::string path = resolve_font_path(ds->font_face, ds->font_bold, ds->font_italic);
	if (path.empty()) {
		if (ds->ft_face)
			return true; // keep using the previously loaded face
		blog(LOG_WARNING, "[obs-datetime-source] could not resolve font '%s'", ds->font_face.c_str());
		return false;
	}

	if (ds->ft_face && ds->loaded_font_path == path && ds->loaded_font_size == ds->font_size)
		return true;

	if (ds->ft_face) {
		FT_Done_Face(ds->ft_face);
		ds->ft_face = nullptr;
	}

	if (FT_New_Face(ds->ft_library, path.c_str(), 0, &ds->ft_face) != 0) {
		blog(LOG_ERROR, "[obs-datetime-source] failed to load font file '%s'", path.c_str());
		ds->ft_face = nullptr;
		return false;
	}

	FT_Set_Pixel_Sizes(ds->ft_face, 0, (FT_UInt)ds->font_size);
	ds->loaded_font_path = path;
	ds->loaded_font_size = ds->font_size;
	return true;
}

// ---------------------------------------------------------------------
// Rasterization: renders `text` into an RGBA texture using FreeType,
// compositing glyphs "over" an optional solid background color.
// ---------------------------------------------------------------------

static void render_text_to_texture(datetime_source *ds, const std::string &text)
{
	if (text.empty() || !ensure_font(ds) || !ds->ft_face) {
		obs_enter_graphics();
		if (ds->texture) {
			gs_texture_destroy(ds->texture);
			ds->texture = nullptr;
		}
		obs_leave_graphics();
		ds->width = 0;
		ds->height = 0;
		return;
	}

	FT_Face face = ds->ft_face;

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
	unpack_color(ds->text_color, text_r, text_g, text_b, text_a);
	float bg_r, bg_g, bg_b, bg_a;
	unpack_color(ds->background_color, bg_r, bg_g, bg_b, bg_a);
	if (!ds->background_enabled)
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
				if (src_a <= 0.0f)
					continue;

				size_t idx = (size_t)y * width + x;
				float dst_a = buf_a[idx];
				float out_a = src_a + dst_a * (1.0f - src_a);
				if (out_a <= 0.0f)
					continue;

				buf_r[idx] = (text_r * src_a + buf_r[idx] * dst_a * (1.0f - src_a)) / out_a;
				buf_g[idx] = (text_g * src_a + buf_g[idx] * dst_a * (1.0f - src_a)) / out_a;
				buf_b[idx] = (text_b * src_a + buf_b[idx] * dst_a * (1.0f - src_a)) / out_a;
				buf_a[idx] = out_a;
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
	if (ds->texture) {
		gs_texture_destroy(ds->texture);
		ds->texture = nullptr;
	}
	ds->texture = gs_texture_create((uint32_t)width, (uint32_t)height, GS_RGBA, 1, &data_ptr, 0);
	obs_leave_graphics();

	ds->width = (uint32_t)width;
	ds->height = (uint32_t)height;
}

// ---------------------------------------------------------------------
// obs_source_info callbacks
// ---------------------------------------------------------------------

static const char *datetime_source_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("DateTimeSource.Name");
}

static void datetime_source_update(void *data, obs_data_t *settings)
{
	datetime_source *ds = (datetime_source *)data;

	ds->mode = (datetime_mode)obs_data_get_int(settings, "mode");
	ds->custom_format = obs_data_get_string(settings, "format");
	ds->text_color = (uint32_t)obs_data_get_int(settings, "color");
	ds->background_enabled = obs_data_get_bool(settings, "background_enabled");
	ds->background_color = (uint32_t)obs_data_get_int(settings, "background_color");

	obs_data_t *font_obj = obs_data_get_obj(settings, "font");
	if (font_obj) {
		ds->font_face = obs_data_get_string(font_obj, "face");
		ds->font_size = (int)obs_data_get_int(font_obj, "size");
		long long flags = obs_data_get_int(font_obj, "flags");
		ds->font_bold = (flags & OBS_FONT_BOLD) != 0;
		ds->font_italic = (flags & OBS_FONT_ITALIC) != 0;
		obs_data_release(font_obj);
	}
	if (ds->font_size <= 0)
		ds->font_size = 64;
	if (ds->font_face.empty())
		ds->font_face = "Sans";

	// Force a re-render on the next tick even if the clock hasn't ticked
	// over to a new second yet.
	ds->last_rendered_time = 0;
	ds->last_rendered_text.clear();
}

static void datetime_source_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "mode", (int)datetime_mode::DATE_AND_TIME);
	obs_data_set_default_string(settings, "format", default_format_for_mode(datetime_mode::DATE_AND_TIME).c_str());
	obs_data_set_default_int(settings, "color", (int)0xFFFFFFFF);
	obs_data_set_default_bool(settings, "background_enabled", false);
	obs_data_set_default_int(settings, "background_color", (int)0xFF000000);

	obs_data_t *font_obj = obs_data_create();
	obs_data_set_default_string(font_obj, "face", "Sans");
	obs_data_set_default_int(font_obj, "size", 96);
	obs_data_set_default_int(font_obj, "flags", 0);
	obs_data_set_default_obj(settings, "font", font_obj);
	obs_data_release(font_obj);
}

static bool mode_modified(obs_properties_t *props, obs_property_t *property, obs_data_t *settings)
{
	UNUSED_PARAMETER(property);
	int mode = (int)obs_data_get_int(settings, "mode");
	obs_property_t *format_prop = obs_properties_get(props, "format");
	obs_property_set_enabled(format_prop, mode == (int)datetime_mode::CUSTOM);
	return true;
}

static obs_properties_t *datetime_source_get_properties(void *data)
{
	datetime_source *ds = (datetime_source *)data;

	obs_properties_t *props = obs_properties_create();

	obs_property_t *mode_list =
		obs_properties_add_list(props, "mode", obs_module_text("Mode"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(mode_list, obs_module_text("Mode.Date"), (int)datetime_mode::DATE_ONLY);
	obs_property_list_add_int(mode_list, obs_module_text("Mode.Time"), (int)datetime_mode::TIME_ONLY);
	obs_property_list_add_int(mode_list, obs_module_text("Mode.DateTime"), (int)datetime_mode::DATE_AND_TIME);
	obs_property_list_add_int(mode_list, obs_module_text("Mode.Custom"), (int)datetime_mode::CUSTOM);
	obs_property_set_modified_callback(mode_list, mode_modified);

	obs_property_t *format_prop = obs_properties_add_text(props, "format", obs_module_text("Format"), OBS_TEXT_DEFAULT);
	if (ds)
		obs_property_set_enabled(format_prop, ds->mode == datetime_mode::CUSTOM);

	obs_properties_add_font(props, "font", obs_module_text("Font"));
	obs_properties_add_color_alpha(props, "color", obs_module_text("Color"));
	obs_properties_add_bool(props, "background_enabled", obs_module_text("BackgroundEnabled"));
	obs_properties_add_color_alpha(props, "background_color", obs_module_text("BackgroundColor"));

	return props;
}

static void *datetime_source_create(obs_data_t *settings, obs_source_t *source)
{
	datetime_source *ds = new datetime_source();
	ds->source = source;

	if (FT_Init_FreeType(&ds->ft_library) != 0) {
		blog(LOG_ERROR, "[obs-datetime-source] failed to initialize FreeType");
		ds->ft_library = nullptr;
	}

	datetime_source_update(ds, settings);
	return ds;
}

static void datetime_source_destroy(void *data)
{
	datetime_source *ds = (datetime_source *)data;

	if (ds->texture) {
		obs_enter_graphics();
		gs_texture_destroy(ds->texture);
		obs_leave_graphics();
	}
	if (ds->ft_face)
		FT_Done_Face(ds->ft_face);
	if (ds->ft_library)
		FT_Done_FreeType(ds->ft_library);

	delete ds;
}

static void datetime_source_video_tick(void *data, float seconds)
{
	UNUSED_PARAMETER(seconds);
	datetime_source *ds = (datetime_source *)data;

	time_t now = time(nullptr);
	if (now == ds->last_rendered_time)
		return;

	std::string text = current_text(ds, now);
	if (text == ds->last_rendered_text) {
		ds->last_rendered_time = now;
		return;
	}

	render_text_to_texture(ds, text);
	ds->last_rendered_text = text;
	ds->last_rendered_time = now;
}

static void datetime_source_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	datetime_source *ds = (datetime_source *)data;
	if (!ds->texture)
		return;

	gs_effect_t *default_effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(default_effect, "image");
	gs_effect_set_texture(image, ds->texture);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA);

	while (gs_effect_loop(default_effect, "Draw"))
		gs_draw_sprite(ds->texture, 0, ds->width, ds->height);

	gs_blend_state_pop();
}

static uint32_t datetime_source_get_width(void *data)
{
	return ((datetime_source *)data)->width;
}

static uint32_t datetime_source_get_height(void *data)
{
	return ((datetime_source *)data)->height;
}

// ---------------------------------------------------------------------
// Registration
//
// Assigned via a static initializer rather than a C-style designated
// initializer, since obs_source_info's field order isn't part of libobs'
// stable ABI/API contract and C++ designated initializers require the
// initializers to appear in declaration order.
// ---------------------------------------------------------------------

struct obs_source_info datetime_source_info = {};

namespace {
struct datetime_source_info_registrar {
	datetime_source_info_registrar()
	{
		datetime_source_info.id = "datetime_text_source";
		datetime_source_info.type = OBS_SOURCE_TYPE_INPUT;
		datetime_source_info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
		datetime_source_info.get_name = datetime_source_get_name;
		datetime_source_info.create = datetime_source_create;
		datetime_source_info.destroy = datetime_source_destroy;
		datetime_source_info.update = datetime_source_update;
		datetime_source_info.get_defaults = datetime_source_get_defaults;
		datetime_source_info.get_properties = datetime_source_get_properties;
		datetime_source_info.video_tick = datetime_source_video_tick;
		datetime_source_info.video_render = datetime_source_video_render;
		datetime_source_info.get_width = datetime_source_get_width;
		datetime_source_info.get_height = datetime_source_get_height;
		datetime_source_info.icon_type = OBS_ICON_TYPE_TEXT;
	}
} g_datetime_source_info_registrar;
} // namespace
