#include "mango/draw/text-node.h"
#include "mango/common/scene_node.h"

#include <cairo.h>
#include <drm_fourcc.h>
#include <glib.h>
#include <pango/pangocairo.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/util/edges.h>

#if defined(__has_include) && __has_include(<scenefx/types/fx/clipped_region.h>)
#include <scenefx/types/fx/clipped_region.h>
#define TEXT_NODE_CORNER_RADIUS 1
#else
#define TEXT_NODE_CORNER_RADIUS 0
#endif

static GHashTable *font_desc_cache = NULL;

PangoFontDescription *get_cached_font_desc(const char *font_desc) {
	if (!font_desc_cache) {
		font_desc_cache =
			g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
								  (GDestroyNotify)pango_font_description_free);
	}

	PangoFontDescription *desc =
		g_hash_table_lookup(font_desc_cache, font_desc);
	if (!desc) {
		desc = pango_font_description_from_string(font_desc);
		g_hash_table_insert(font_desc_cache, g_strdup(font_desc), desc);
	}
	return desc;
}

void mango_text_global_finish(void) {
	if (font_desc_cache) {
		g_hash_table_destroy(font_desc_cache);
		font_desc_cache = NULL;
	}
	pango_cairo_font_map_set_default(NULL);
}

void text_buffer_destroy(struct wlr_buffer *wlr_buffer) {
	struct mango_text_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	if (buf->surface) {
		cairo_surface_destroy(buf->surface);
	}
	free(buf);
}

bool text_buffer_begin_data_ptr_access(struct wlr_buffer *wlr_buffer,
									   uint32_t flags, void **data,
									   uint32_t *format, size_t *stride) {
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) {
		return false;
	}

	struct mango_text_buffer *buf = wl_container_of(wlr_buffer, buf, base);
	if (!buf->surface) {
		return false;
	}
	*data = cairo_image_surface_get_data(buf->surface);
	*format = DRM_FORMAT_ARGB8888;
	*stride = cairo_image_surface_get_stride(buf->surface);
	return true;
}

void text_buffer_end_data_ptr_access(struct wlr_buffer *wlr_buffer) {}

static const struct wlr_buffer_impl text_buffer_impl = {
	.destroy = text_buffer_destroy,
	.begin_data_ptr_access = text_buffer_begin_data_ptr_access,
	.end_data_ptr_access = text_buffer_end_data_ptr_access,
};

/* Scales neighbouring corner radii down so both arcs fit along their shared
 * edge. A radius-0 corner has no arc, so its neighbour may take the full
 * edge. */
static struct fx_corner_radii clamp_corners(struct fx_corner_radii corners,
											int32_t width, int32_t height) {
	struct {
		uint16_t a, b;
		int32_t edge;
	} pairs[] = {
		{corners.top_left, corners.top_right, width},
		{corners.bottom_left, corners.bottom_right, width},
		{corners.top_left, corners.bottom_left, height},
		{corners.top_right, corners.bottom_right, height},
	};

	float scale = 1.0f;
	for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
		if (pairs[i].a == 0 || pairs[i].b == 0)
			continue;

		int32_t sum = (int32_t)pairs[i].a + (int32_t)pairs[i].b;
		if (sum > pairs[i].edge) {
			float fit = (float)pairs[i].edge / (float)sum;
			if (fit < scale)
				scale = fit;
		}
	}

	if (scale >= 1.0f)
		return corners;

	struct fx_corner_radii clamped = corners;
	uint16_t *values[4] = {&clamped.top_left, &clamped.top_right,
						   &clamped.bottom_right, &clamped.bottom_left};
	for (size_t i = 0; i < 4; i++)
		*values[i] = (uint16_t)(*values[i] * scale);
	return clamped;
}

static void rect_apply(struct wlr_scene_rect *rect, const float color[4],
					   struct fx_corner_radii corners, int32_t x, int32_t y,
					   int32_t width, int32_t height) {
	float premultiplied[4] = {
		color[0] * color[3],
		color[1] * color[3],
		color[2] * color[3],
		color[3],
	};
	wlr_scene_rect_set_color(rect, premultiplied);

	if (width < 0) {
		width = 0;
	}
	if (height < 0) {
		height = 0;
	}

#if TEXT_NODE_CORNER_RADIUS
	wlr_scene_rect_set_corner_radii(rect,
									clamp_corners(corners, width, height));
#endif

	wlr_scene_node_set_position(&rect->node, x, y);
	wlr_scene_rect_set_size(rect, width, height);
}

// Inner-edge radius of a border: drawing the inset rect with the outer radius
// would cover the border's width along the curve.
static int32_t inner_corner_radius(int32_t outer_radius, int32_t border) {
	int32_t inner = outer_radius - border;
	return inner > 0 ? inner : 0;
}

static struct fx_corner_radii inner_corners_of(struct fx_corner_radii outer,
											   int32_t border) {
	struct fx_corner_radii inner =
		corner_radii_new(inner_corner_radius(outer.top_left, border),
						 inner_corner_radius(outer.top_right, border),
						 inner_corner_radius(outer.bottom_right, border),
						 inner_corner_radius(outer.bottom_left, border));
	/* The inner edge is the same curve, so it keeps the exponent. */
	inner.exponent = outer.exponent;
	return inner;
}

/* A fade scales only the alpha: rect_apply() premultiplies the colour
 * afterwards. */
static const float *bar_faded_color(const MangoBarDecoration *node,
									const float color[4], float out[4]) {
	memcpy(out, color, 4 * sizeof(float));
	if (node->opacity < 1.0f) {
		out[3] *= node->opacity;
	}
	return out;
}

static void rect_apply_border(struct wlr_scene_rect *rect, const float color[4],
							  struct fx_corner_radii outer,
							  struct fx_corner_radii inner, int32_t border,
							  int32_t x, int32_t y, int32_t width,
							  int32_t height) {
	rect_apply(rect, color, outer, x, y, width, height);

#if TEXT_NODE_CORNER_RADIUS
	int32_t inner_w = width - 2 * border;
	int32_t inner_h = height - 2 * border;
	if (border > 0 && inner_w > 0 && inner_h > 0) {
		struct clipped_region clipped = clipped_region_get_default();
		clipped.area = (struct wlr_box){
			.x = border,
			.y = border,
			.width = inner_w,
			.height = inner_h,
		};
		clipped.corners = clamp_corners(inner, inner_w, inner_h);
		wlr_scene_rect_set_clipped_region(rect, clipped);
	} else {
		wlr_scene_rect_set_clipped_region(rect, clipped_region_get_default());
	}
#endif
}

static void layout_configure(PangoLayout *layout, const char *font_desc,
							 const char *text) {
	pango_layout_set_font_description(layout, get_cached_font_desc(font_desc));
	pango_layout_set_text(layout, text, -1);
}

static void measure_init(struct mango_text_measure *measure) {
	measure->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, 1, 1);
	measure->cr = cairo_create(measure->surface);
	measure->context = pango_cairo_create_context(measure->cr);
	measure->layout = pango_layout_new(measure->context);
	measure->scale = 1.0f;
}

static void measure_finish(struct mango_text_measure *measure) {
	if (measure->layout) {
		g_object_unref(measure->layout);
		measure->layout = NULL;
	}
	if (measure->context) {
		g_object_unref(measure->context);
		measure->context = NULL;
	}
	if (measure->cr) {
		cairo_destroy(measure->cr);
		measure->cr = NULL;
	}
	if (measure->surface) {
		cairo_surface_destroy(measure->surface);
		measure->surface = NULL;
	}
}

static void measure_text(struct mango_text_measure *measure,
						 const char *font_desc, const char *text, float scale,
						 int32_t *out_pixel_w, int32_t *out_pixel_h) {
	if (scale <= 0.0f) {
		scale = 1.0f;
	}
	if (measure->scale != scale) {
		pango_cairo_context_set_resolution(measure->context, 96.0 * scale);
		measure->scale = scale;
	}

	layout_configure(measure->layout, font_desc, text);
	pango_layout_get_pixel_size(measure->layout, out_pixel_w, out_pixel_h);
}

static char *text_ellipsize_line(struct mango_text_measure *measure,
								 const char *font_desc, const char *text,
								 float scale, int32_t max_pixel_w) {
	int32_t pixel_w = 0, pixel_h = 0;
	int32_t best = 0;
	const char *end = text + strlen(text);

	measure_text(measure, font_desc, "…", scale, &pixel_w, &pixel_h);
	if (pixel_w > max_pixel_w) {
		return g_strdup("…");
	}

	for (const char *pos = g_utf8_next_char(text); pos <= end;
		 pos = g_utf8_next_char(pos)) {
		int32_t length = (int32_t)(pos - text);
		char *candidate = g_strdup_printf("%.*s…", length, text);
		measure_text(measure, font_desc, candidate, scale, &pixel_w, &pixel_h);
		g_free(candidate);
		if (pixel_w > max_pixel_w) {
			break;
		}
		best = length;
	}

	return g_strdup_printf("%.*s…", best, text);
}

static char *text_ellipsize(struct mango_text_measure *measure,
							const char *font_desc, const char *text,
							float scale, int32_t max_pixel_w) {
	if (!strchr(text, '\n'))
		return text_ellipsize_line(measure, font_desc, text, scale,
								   max_pixel_w);

	GString *result = g_string_new(NULL);
	const char *line = text;

	while (true) {
		const char *newline = strchr(line, '\n');
		size_t length = newline ? (size_t)(newline - line) : strlen(line);
		char *part = g_strndup(line, length);
		int32_t pixel_w = 0, pixel_h = 0;
		measure_text(measure, font_desc, part, scale, &pixel_w, &pixel_h);
		if (pixel_w <= max_pixel_w) {
			g_string_append(result, part);
		} else {
			char *ellipsized = text_ellipsize_line(measure, font_desc, part,
												   scale, max_pixel_w);
			g_string_append(result, ellipsized);
			g_free(ellipsized);
		}
		g_free(part);

		if (!newline)
			break;
		g_string_append_c(result, '\n');
		line = newline + 1;
	}

	return g_string_free(result, false);
}

static cairo_surface_t *
text_surface_create(struct mango_text_measure *measure, const char *font_desc,
					const char *text, float scale, const float color[4],
					int32_t *out_pixel_w, int32_t *out_pixel_h) {
	if (scale <= 0.0f) {
		scale = 1.0f;
	}

	int32_t width = 0, height = 0;
	measure_text(measure, font_desc, text, scale, &width, &height);
	*out_pixel_w = 0;
	*out_pixel_h = 0;
	if (width <= 0 || height <= 0) {
		return NULL;
	}

	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		return NULL;
	}

	cairo_t *cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	PangoContext *context = pango_cairo_create_context(cr);
	pango_cairo_context_set_resolution(context, 96.0 * scale);
	PangoLayout *layout = pango_layout_new(context);
	layout_configure(layout, font_desc, text);
	cairo_move_to(cr, 0.0, 0.0);
	cairo_set_source_rgba(cr, color[0], color[1], color[2], color[3]);
	pango_cairo_show_layout(cr, layout);
	cairo_surface_flush(surface);

	g_object_unref(layout);
	g_object_unref(context);
	cairo_destroy(cr);

	*out_pixel_w = width;
	*out_pixel_h = height;
	return surface;
}

static cairo_surface_t *text_surface_create_two_lines(
	struct mango_text_measure *measure, const char *font_desc,
	const char *first, const char *second, float scale, const float color[4],
	int32_t *out_pixel_w, int32_t *out_pixel_h) {
	int32_t first_w, first_h, second_w, second_h;
	cairo_surface_t *first_surface = text_surface_create(
		measure, font_desc, first, scale, color, &first_w, &first_h);
	cairo_surface_t *second_surface = text_surface_create(
		measure, font_desc, second, scale, color, &second_w, &second_h);
	if (!first_surface || !second_surface) {
		if (first_surface)
			cairo_surface_destroy(first_surface);
		if (second_surface)
			cairo_surface_destroy(second_surface);
		return NULL;
	}

	int32_t width = first_w > second_w ? first_w : second_w;
	int32_t height = first_h + second_h;
	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		cairo_surface_destroy(first_surface);
		cairo_surface_destroy(second_surface);
		return NULL;
	}

	cairo_t *cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	cairo_set_source_surface(cr, first_surface, 0, 0);
	cairo_paint(cr);
	cairo_set_source_surface(cr, second_surface, (width - second_w) / 2,
							 first_h);
	cairo_paint(cr);
	cairo_surface_flush(surface);

	cairo_destroy(cr);
	cairo_surface_destroy(first_surface);
	cairo_surface_destroy(second_surface);

	*out_pixel_w = width;
	*out_pixel_h = height;
	return surface;
}

static void text_node_install(struct wlr_scene_buffer *scene_buffer,
							  struct mango_text_buffer **slot,
							  cairo_surface_t *surface, int32_t pixel_w,
							  int32_t pixel_h) {
	if (*slot) {
		wlr_buffer_drop(&(*slot)->base);
		*slot = NULL;
	}

	if (!surface) {
		wlr_scene_buffer_set_buffer(scene_buffer, NULL);
		wlr_scene_buffer_set_dest_size(scene_buffer, 0, 0);
		return;
	}

	struct mango_text_buffer *buf = calloc(1, sizeof(*buf));
	if (!buf) {
		cairo_surface_destroy(surface);
		return;
	}
	wlr_buffer_init(&buf->base, &text_buffer_impl, pixel_w, pixel_h);
	buf->surface = surface;
	*slot = buf;

	wlr_scene_buffer_set_buffer(scene_buffer, &buf->base);
}

static void text_node_apply_size(struct wlr_scene_buffer *scene_buffer,
								 int32_t pixel_w, int32_t pixel_h, float scale,
								 int32_t max_logical_w, int32_t max_logical_h,
								 int32_t *out_logical_w,
								 int32_t *out_logical_h) {
	if (scale <= 0.0f) {
		scale = 1.0f;
	}

	int32_t clip_pixel_w = pixel_w;
	int32_t clip_pixel_h = pixel_h;

	if (max_logical_w > 0) {
		int32_t max_pixel_w = (int32_t)(max_logical_w * scale + 0.5f);
		if (max_pixel_w < clip_pixel_w) {
			clip_pixel_w = max_pixel_w;
		}
	}
	if (max_logical_h > 0) {
		int32_t max_pixel_h = (int32_t)(max_logical_h * scale + 0.5f);
		if (max_pixel_h < clip_pixel_h) {
			clip_pixel_h = max_pixel_h;
		}
	}
	if (clip_pixel_w < 0) {
		clip_pixel_w = 0;
	}
	if (clip_pixel_h < 0) {
		clip_pixel_h = 0;
	}

	if (pixel_w > 0 && (clip_pixel_w < pixel_w || clip_pixel_h < pixel_h)) {
		const struct wlr_fbox box = {
			.width = clip_pixel_w,
			.height = clip_pixel_h,
		};
		wlr_scene_buffer_set_source_box(scene_buffer, &box);
	} else {
		wlr_scene_buffer_set_source_box(scene_buffer, NULL);
	}

	int32_t logical_w = (int32_t)(clip_pixel_w / scale + 0.5f);
	int32_t logical_h = (int32_t)(clip_pixel_h / scale + 0.5f);
	wlr_scene_buffer_set_dest_size(scene_buffer, logical_w, logical_h);

	*out_logical_w = logical_w;
	*out_logical_h = logical_h;
}

static void text_node_clear(struct wlr_scene_buffer *scene_buffer,
							struct mango_text_buffer **slot,
							struct wlr_scene_rect *border,
							struct wlr_scene_rect *bg) {
	text_node_install(scene_buffer, slot, NULL, 0, 0);
	wlr_scene_buffer_set_source_box(scene_buffer, NULL);
	wlr_scene_rect_set_size(border, 0, 0);
	wlr_scene_rect_set_size(bg, 0, 0);
#if TEXT_NODE_CORNER_RADIUS
	wlr_scene_rect_set_clipped_region(border, clipped_region_get_default());
#endif
}

MangoJumpLabel *mango_jump_label_node_create(struct wlr_scene_tree *parent,
											 DecorateDrawData data) {
	MangoJumpLabel *node = calloc(1, sizeof(*node));
	if (!node)
		return NULL;

	node->scene = wlr_scene_tree_create(parent);
	if (!node->scene) {
		free(node);
		return NULL;
	}

	node->border = wlr_scene_rect_create(node->scene, 0, 0, (float[4]){0});
	node->bg = wlr_scene_rect_create(node->scene, 0, 0, (float[4]){0});
	node->scene_buffer = wlr_scene_buffer_create(node->scene, NULL);
	if (!node->border || !node->bg || !node->scene_buffer) {
		wlr_scene_node_destroy(&node->scene->node);
		free(node);
		return NULL;
	}

	memcpy(node->fg_color, data.fg_color, sizeof(node->fg_color));
	memcpy(node->bg_color, data.bg_color, sizeof(node->bg_color));
	memcpy(node->focus_fg_color, data.focus_fg_color,
		   sizeof(node->focus_fg_color));
	memcpy(node->focus_bg_color, data.focus_bg_color,
		   sizeof(node->focus_bg_color));
	memcpy(node->border_color, data.border_color, sizeof(node->border_color));
	node->border_width = data.border_width;
	node->corner_radius = data.corner_radius;
	node->padding_x = data.padding_x;
	node->padding_y = data.padding_y;
	node->font_desc =
		g_strdup(data.font_desc ? data.font_desc : "monospace Bold 16");

	node->cached_scale = -1.0f;
	node->scene_buffer->node.data = NULL;
	measure_init(&node->measure);

	return node;
}

void mango_jump_label_node_destroy(MangoJumpLabel *node) {
	if (!node)
		return;

	if (node->buffer) {
		wlr_buffer_drop(&node->buffer->base);
		node->buffer = NULL;
	}
	if (node->scene) {
		wlr_scene_node_destroy(&node->scene->node);
		node->scene = NULL;
	}

	measure_finish(&node->measure);

	g_free(node->font_desc);
	g_free(node->cached_text);
	g_free(node->cached_font_desc);

	free(node);
}

static void jump_label_apply_geometry(MangoJumpLabel *node) {
	float scale = node->cached_scale > 0.0f ? node->cached_scale : 1.0f;

	text_node_apply_size(node->scene_buffer, node->surface_pixel_w,
						 node->surface_pixel_h, scale, 0, 0,
						 &node->text_logical_w, &node->text_logical_h);

	if (node->text_logical_w <= 0 || node->text_logical_h <= 0) {
		wlr_scene_rect_set_size(node->border, 0, 0);
		wlr_scene_rect_set_size(node->bg, 0, 0);
		node->logical_width = 0;
		node->logical_height = 0;
		return;
	}

	int32_t border = node->border_width > 0 ? node->border_width : 0;
	int32_t width = node->text_logical_w + 2 * node->padding_x + 2 * border;
	int32_t height = node->text_logical_h + 2 * node->padding_y + 2 * border;

	node->logical_width = width;
	node->logical_height = height;

	struct fx_corner_radii outer = corner_radii_all(node->corner_radius);
	struct fx_corner_radii inner = inner_corners_of(outer, border);

	rect_apply_border(node->border, node->border_color, outer, inner, border, 0,
					  0, width, height);
	rect_apply(node->bg, node->focused ? node->focus_bg_color : node->bg_color,
			   inner, border, border, width - 2 * border, height - 2 * border);

	wlr_scene_node_set_position(&node->scene_buffer->node,
								border + node->padding_x,
								border + node->padding_y);
}

void mango_jump_label_node_update(MangoJumpLabel *node, const char *text,
								  float scale) {
	if (!node || !text)
		return;
	if (scale <= 0.0f) {
		scale = 1.0f;
	}

	const float *fg_color =
		node->focused ? node->focus_fg_color : node->fg_color;

	bool dirty =
		node->cached_scale != scale || !node->cached_text ||
		strcmp(node->cached_text, text) != 0 || !node->cached_font_desc ||
		strcmp(node->cached_font_desc, node->font_desc) != 0 ||
		memcmp(node->cached_fg_color, fg_color, sizeof(node->fg_color)) != 0 ||
		node->cached_focused != node->focused;

	if (dirty) {
		g_free(node->cached_text);
		node->cached_text = g_strdup(text);
		g_free(node->cached_font_desc);
		node->cached_font_desc = g_strdup(node->font_desc);
		node->cached_scale = scale;
		memcpy(node->cached_fg_color, fg_color, sizeof(node->cached_fg_color));
		node->cached_focused = node->focused;

		cairo_surface_t *surface = text_surface_create(
			&node->measure, node->font_desc, text, scale, fg_color,
			&node->surface_pixel_w, &node->surface_pixel_h);
		text_node_install(node->scene_buffer, &node->buffer, surface,
						  node->surface_pixel_w, node->surface_pixel_h);
	}

	jump_label_apply_geometry(node);
}

void mango_jump_label_node_set_focus(MangoJumpLabel *node, bool focused) {
	if (!node || node->focused == focused)
		return;
	node->focused = focused;
	if (node->cached_text) {
		mango_jump_label_node_update(
			node, node->cached_text,
			node->cached_scale > 0.0f ? node->cached_scale : 1.0f);
	}
}

void mango_jump_label_node_set_background(MangoJumpLabel *node, float r,
										  float g, float b, float a) {
	if (!node)
		return;
	node->bg_color[0] = r;
	node->bg_color[1] = g;
	node->bg_color[2] = b;
	node->bg_color[3] = a;
	if (node->cached_text) {
		mango_jump_label_node_update(
			node, node->cached_text,
			node->cached_scale > 0.0f ? node->cached_scale : 1.0f);
	}
}

void mango_jump_label_node_set_border(MangoJumpLabel *node, float r, float g,
									  float b, float a, int32_t width,
									  int32_t radius) {
	if (!node)
		return;
	node->border_color[0] = r;
	node->border_color[1] = g;
	node->border_color[2] = b;
	node->border_color[3] = a;
	node->border_width = width > 0 ? width : 0;
	node->corner_radius = radius;
	if (node->cached_text) {
		mango_jump_label_node_update(
			node, node->cached_text,
			node->cached_scale > 0.0f ? node->cached_scale : 1.0f);
	}
}

void mango_jump_label_node_set_padding(MangoJumpLabel *node, int32_t pad_x,
									   int32_t pad_y) {
	if (!node)
		return;
	node->padding_x = pad_x >= 0 ? pad_x : 0;
	node->padding_y = pad_y >= 0 ? pad_y : 0;
	if (node->cached_text) {
		mango_jump_label_node_update(
			node, node->cached_text,
			node->cached_scale > 0.0f ? node->cached_scale : 1.0f);
	}
}

MangoBarDecoration *mango_bar_decoration_create(void *cdata, uint32_t type,
												bool is_tab,
												struct wlr_scene_tree *parent,
												DecorateDrawData data,
												int32_t width, int32_t height) {
	MangoBarDecoration *mangobar = calloc(1, sizeof(*mangobar));
	if (!mangobar)
		return NULL;

	mangobar->scene = wlr_scene_tree_create(parent);
	if (!mangobar->scene) {
		free(mangobar);
		return NULL;
	}

	mangobar->border =
		wlr_scene_rect_create(mangobar->scene, 0, 0, (float[4]){0});
	mangobar->bg = wlr_scene_rect_create(mangobar->scene, 0, 0, (float[4]){0});
	mangobar->scene_buffer = wlr_scene_buffer_create(mangobar->scene, NULL);
	mangobar->close_buffer = wlr_scene_buffer_create(mangobar->scene, NULL);
	if (!mangobar->border || !mangobar->bg || !mangobar->scene_buffer ||
		!mangobar->close_buffer) {
		wlr_scene_node_destroy(&mangobar->scene->node);
		free(mangobar);
		return NULL;
	}
	wlr_scene_node_set_enabled(&mangobar->close_buffer->node, false);

	mangobar->hover_button = -1;
	for (int32_t i = 0; i < MANGO_BAR_BUTTON_COUNT; i++) {
		MangoBarButton *button = &mangobar->buttons[i];
		button->scene_buffer = wlr_scene_buffer_create(mangobar->scene, NULL);
		if (!button->scene_buffer) {
			wlr_scene_node_destroy(&mangobar->scene->node);
			free(mangobar);
			return NULL;
		}
		wlr_scene_node_set_enabled(&button->scene_buffer->node, false);

		/* MacOS-like traffic lights: close red, minimize yellow,
		 * maximize green. */
		switch (i) {
		case MANGO_BAR_BUTTON_CLOSE:
			button->color[0] = 0xff / 255.0f;
			button->color[1] = 0x5f / 255.0f;
			button->color[2] = 0x57 / 255.0f;
			break;
		case MANGO_BAR_BUTTON_MINIMIZE:
			button->color[0] = 0xfe / 255.0f;
			button->color[1] = 0xbc / 255.0f;
			button->color[2] = 0x2e / 255.0f;
			break;
		default:
			button->color[0] = 0x28 / 255.0f;
			button->color[1] = 0xc8 / 255.0f;
			button->color[2] = 0x40 / 255.0f;
			break;
		}
		button->color[3] = 1.0f;
		for (int c = 0; c < 3; c++)
			button->hover_color[c] =
				button->color[c] + (1.0f - button->color[c]) * 0.4f;
		button->hover_color[3] = 1.0f;

		/* The glyph is a dark tint of the circle so it reads on all
		 * three colors. */
		button->glyph_color[0] = button->color[0] * 0.35f;
		button->glyph_color[1] = button->color[1] * 0.35f;
		button->glyph_color[2] = button->color[2] * 0.35f;
		button->glyph_color[3] = 1.0f;
	}

	memcpy(mangobar->fg_color, data.fg_color, sizeof(mangobar->fg_color));
	memcpy(mangobar->bg_color, data.bg_color, sizeof(mangobar->bg_color));
	memcpy(mangobar->focus_fg_color, data.focus_fg_color,
		   sizeof(mangobar->focus_fg_color));
	memcpy(mangobar->focus_bg_color, data.focus_bg_color,
		   sizeof(mangobar->focus_bg_color));
	memcpy(mangobar->border_color, data.border_color,
		   sizeof(mangobar->border_color));
	mangobar->border_width = data.border_width;
	mangobar->corner_radius = data.corner_radius;
	mangobar->padding_x = data.padding_x;
	mangobar->padding_y = data.padding_y;
	mangobar->font_desc =
		g_strdup(data.font_desc ? data.font_desc : "monospace Bold 13");

	mangobar->target_width = width;
	mangobar->target_height = height;
	mangobar->type = type;
	mangobar->is_tab = is_tab;
	mangobar->node_data = cdata;

	mangobar->cached_scale = -1.0f;
	mangobar->opacity = 1.0f;

	mangobar->close_color[0] = 0.68f;
	mangobar->close_color[1] = 0.25f;
	mangobar->close_color[2] = 0.12f;
	mangobar->close_color[3] = 1.0f;
	mangobar->close_hover_color[0] = 0.81f;
	mangobar->close_hover_color[1] = 0.55f;
	mangobar->close_hover_color[2] = 0.47f;
	mangobar->close_hover_color[3] = 1.0f;
	mangobar->close_x_color[0] = mangobar->close_x_color[1] =
		mangobar->close_x_color[2] = mangobar->close_x_color[3] = 1.0f;

	mango_scene_node_set(&mangobar->scene->node, mangobar->type, mangobar);
	measure_init(&mangobar->measure);

	return mangobar;
}

void mango_bar_decoration_destroy(MangoBarDecoration *node) {
	if (!node)
		return;

	if (node->buffer) {
		wlr_buffer_drop(&node->buffer->base);
		node->buffer = NULL;
	}
	if (node->close_texture) {
		wlr_buffer_drop(&node->close_texture->base);
		node->close_texture = NULL;
	}
	for (int32_t i = 0; i < MANGO_BAR_BUTTON_COUNT; i++) {
		if (node->buttons[i].texture) {
			wlr_buffer_drop(&node->buttons[i].texture->base);
			node->buttons[i].texture = NULL;
		}
	}
	if (node->scene) {
		wlr_scene_node_destroy(&node->scene->node);
		node->scene = NULL;
	}

	measure_finish(&node->measure);

	g_free(node->font_desc);
	g_free(node->cached_text);
	g_free(node->cached_font_desc);
	g_free(node->last_text);
	free(node);
}

/* Width of the traffic-light strip: all buttons plus the gaps between them. */
static int32_t bar_buttons_strip_width(const MangoBarDecoration *node) {
	if (!node->buttons_enabled || node->button_size <= 0)
		return 0;

	int32_t strip = MANGO_BAR_BUTTON_COUNT * node->button_size +
					(MANGO_BAR_BUTTON_COUNT - 1) * node->button_gap;
	return strip;
}

static void bar_clip_buffer(MangoBarDecoration *node,
							struct wlr_scene_buffer *buffer);
static void bar_decoration_apply_geometry(MangoBarDecoration *node);

/* Square box a traffic-light button occupies. */
static bool bar_button_box(const MangoBarDecoration *node, int32_t index,
						   int32_t *out_x, int32_t *out_y, int32_t *out_size) {
	if (!node->buttons_enabled || index < 0 ||
		index >= MANGO_BAR_BUTTON_COUNT || node->button_size <= 0 ||
		node->target_width <= 0 || node->target_height <= 0)
		return false;

	int32_t size = node->button_size;
	if (size > node->target_height)
		size = node->target_height;
	if (size > node->target_width)
		size = node->target_width;

	int32_t strip = bar_buttons_strip_width(node);
	int32_t margin = node->button_margin;
	int32_t space = node->target_width - strip;
	if (space < 0)
		space = 0;
	if (margin > space)
		margin = space;

	*out_size = size;
	/* The strip hangs off the left or the right edge (buttons_left) and lists
	 * close, minimize, maximize inwards from that edge, or in the reverse order
	 * when buttons_inverse is set. */
	int32_t slot =
		node->buttons_inverse ? MANGO_BAR_BUTTON_COUNT - 1 - index : index;
	int32_t x = node->buttons_left ? margin + slot * (size + node->button_gap)
								   : node->target_width - margin - size -
										 slot * (size + node->button_gap);
	/* Clamp inside the bar: on a narrow bar the button strip can be wider
	 * than the bar itself. */
	int32_t room = node->target_width - size;
	if (room < 0)
		room = 0;
	if (x > room)
		x = room;
	if (x < 0)
		x = 0;
	*out_x = x;
	*out_y = (node->target_height - size) / 2;
	return true;
}
static const float bar_button_unfocused_color[4] = {
	0x44 / 255.0f, 0x48 / 255.0f, 0x4d / 255.0f, 1.0f};

static void bar_button_render(MangoBarDecoration *node, int32_t index,
							  int32_t size, int32_t pixel, float scale,
							  bool hover) {
	MangoBarButton *button = &node->buttons[index];

	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixel, pixel);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		text_node_install(button->scene_buffer, &button->texture, NULL, 0, 0);
		return;
	}

	cairo_t *cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	double center = pixel / 2.0;
	/* A hovered window wakes the lights up: hovering the strip counts as
	 * focused, so the discs stay colorful and the icons appear. */
	bool lit = node->focused || node->buttons_hover;
	const float *circle = !lit ? bar_button_unfocused_color
							   : (hover ? button->hover_color : button->color);
	cairo_set_source_rgba(cr, circle[0], circle[1], circle[2], circle[3]);
	cairo_arc(cr, center, center, center, 0, 2 * 3.14159265358979323846);
	cairo_fill(cr);

	if (!lit) {
		cairo_surface_flush(surface);
		cairo_destroy(cr);
		text_node_install(button->scene_buffer, &button->texture, surface,
						  pixel, pixel);
		button->cached_size = size;
		button->cached_scale = scale;
		button->cached_hover = hover;
		button->cached_buttons_hover = node->buttons_hover;
		button->cached_focused = node->focused;
		button->cached_valid = true;
		return;
	}

	double line_width = pixel * 0.12;
	if (line_width < 1.0)
		line_width = 1.0;
	cairo_set_line_width(cr, line_width);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_source_rgba(cr, button->glyph_color[0], button->glyph_color[1],
						  button->glyph_color[2], button->glyph_color[3]);

	double inset = pixel * 0.32;
	double lo = inset, hi = pixel - inset;

	switch (index) {
	case MANGO_BAR_BUTTON_CLOSE:
		cairo_move_to(cr, lo, lo);
		cairo_line_to(cr, hi, hi);
		cairo_move_to(cr, hi, lo);
		cairo_line_to(cr, lo, hi);
		cairo_stroke(cr);
		break;
	case MANGO_BAR_BUTTON_MINIMIZE:
		cairo_move_to(cr, lo, center);
		cairo_line_to(cr, hi, center);
		cairo_stroke(cr);
		break;
	case MANGO_BAR_BUTTON_MAXIMIZE: {
		double half = pixel * 0.20;
		cairo_set_line_width(cr, line_width);
		cairo_rectangle(cr, center - half, center - half, half * 2, half * 2);
		cairo_stroke(cr);
		break;
	}
	default:
		break;
	}

	cairo_surface_flush(surface);
	cairo_destroy(cr);

	text_node_install(button->scene_buffer, &button->texture, surface, pixel,
					  pixel);

	button->cached_size = size;
	button->cached_scale = scale;
	button->cached_hover = hover;
	button->cached_buttons_hover = node->buttons_hover;
	button->cached_focused = node->focused;
	button->cached_valid = true;
}

static void bar_buttons_sync(MangoBarDecoration *node) {
	float scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;

	for (int32_t i = 0; i < MANGO_BAR_BUTTON_COUNT; i++) {
		MangoBarButton *button = &node->buttons[i];
		if (!button->scene_buffer)
			continue;

		int32_t x = 0, y = 0, size = 0;
		if (!bar_button_box(node, i, &x, &y, &size)) {
			text_node_install(button->scene_buffer, &button->texture, NULL, 0,
							  0);
			button->cached_valid = false;
			wlr_scene_node_set_enabled(&button->scene_buffer->node, false);
			continue;
		}

		int32_t pixel = (int32_t)(size * scale + 0.5f);
		if (pixel < 1)
			pixel = 1;

		bool hover = node->hover_button == i;
		if (!button->cached_valid || button->cached_size != size ||
			button->cached_scale != scale || button->cached_hover != hover ||
			button->cached_buttons_hover != node->buttons_hover ||
			button->cached_focused != node->focused) {
			bar_button_render(node, i, size, pixel, scale, hover);
		}

		wlr_scene_buffer_set_dest_size(button->scene_buffer, size, size);
		wlr_scene_node_set_position(&button->scene_buffer->node, x, y);
		bar_clip_buffer(node, button->scene_buffer);
		wlr_scene_node_set_enabled(&button->scene_buffer->node, true);
	}
}

/* Side inset of the bar's contents; reserves room for the button strip so
 * the title stays centered. */
static int32_t bar_decoration_side_inset(const MangoBarDecoration *node) {
	int32_t inset = node->border_width > 0 ? node->border_width : 0;
	inset += node->padding_x;

	int32_t strip = bar_buttons_strip_width(node);
	if (strip > 0) {
		int32_t reserved = strip + node->button_margin;
		if (reserved > inset) {
			inset = reserved;
		}
	}

	return inset;
}

static int32_t bar_decoration_avail_width(MangoBarDecoration *node) {
	int32_t avail = node->target_width - 2 * bar_decoration_side_inset(node);
	return avail > 0 ? avail : 0;
}
static struct fx_corner_radii
bar_outer_corners(const MangoBarDecoration *node) {
	if (node->corners_override)
		return node->corners;
	return corner_radii_all(node->corner_radius);
}

void mango_bar_decoration_set_corners(MangoBarDecoration *node,
									  struct fx_corner_radii corners) {
	if (!node)
		return;
	/* Called from the per-frame draw path, so a no-op must stay cheap. */
	if (node->corners_override &&
		memcmp(&node->corners, &corners, sizeof(corners)) == 0)
		return;

	node->corners_override = true;
	node->corners = corners;
	if (node->cached_text) {
		float scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;
		mango_bar_decoration_update(node, node->last_text, scale);
	}
}

void mango_bar_decoration_set_snap_edges(MangoBarDecoration *node,
										 uint32_t edges) {
	if (!node || node->snap_edges == edges)
		return;
	node->snap_edges = edges;
}

void mango_bar_decoration_set_opacity(MangoBarDecoration *node, float opacity) {
	if (!node) {
		return;
	}

	if (opacity < 0.0f) {
		opacity = 0.0f;
	} else if (opacity > 1.0f) {
		opacity = 1.0f;
	}
	if (node->opacity == opacity) {
		return;
	}
	node->opacity = opacity;

	/* Buffers carry their own opacity; the rects get theirs via the geometry
	 * pass. */
	wlr_scene_buffer_set_opacity(node->scene_buffer, opacity);
	wlr_scene_buffer_set_opacity(node->close_buffer, opacity);
	for (int32_t i = 0; i < MANGO_BAR_BUTTON_COUNT; i++) {
		if (node->buttons[i].scene_buffer) {
			wlr_scene_buffer_set_opacity(node->buttons[i].scene_buffer,
										 opacity);
		}
	}

	bar_decoration_apply_geometry(node);
}

void mango_bar_decoration_set_visible(MangoBarDecoration *node, int32_t x,
									  int32_t y, int32_t width,
									  int32_t height) {
	if (!node)
		return;

	if (width < 0)
		width = 0;
	if (height < 0)
		height = 0;

	struct wlr_box visible = {x, y, width, height};
	if (wlr_box_equal(&node->visible, &visible))
		return;
	node->visible = visible;

	/* Called from the per-frame draw path, so this only has to redo the cheap
	 * geometry pass - the text surface itself does not change. */
	bar_decoration_apply_geometry(node);
}

/* Clip `buffer` to the part of the bar that is still on screen. */
static void bar_clip_buffer(MangoBarDecoration *node,
							struct wlr_scene_buffer *buffer) {
#if TEXT_NODE_CORNER_RADIUS
	struct wlr_box *visible = &node->visible;
	struct clipped_region clip = clipped_region_get_default();
	if (visible->width > 0 && visible->height > 0) {
		/* A scene buffer's clip is in the buffer's own space: the scene adds
		 * the node's position to get the absolute box back. */
		struct wlr_box box = {
			.x = visible->x - buffer->node.x,
			.y = visible->y - buffer->node.y,
			.width = visible->width,
			.height = visible->height,
		};
		struct wlr_box whole = {0, 0, buffer->dst_width, buffer->dst_height};
		if (!wlr_box_contains_box(&box, &whole)) {
			clip.area = box;
			/* Hard clip: there is nothing under the edge to fade into. */
			clip.soft = false;
		}
	}
	wlr_scene_buffer_set_clipped_region(buffer, clip);
#endif
}

static void bar_decoration_apply_geometry(MangoBarDecoration *node) {
	int32_t border = node->border_width > 0 ? node->border_width : 0;
	int32_t width = node->target_width > 0 ? node->target_width : 0;
	int32_t height = node->target_height > 0 ? node->target_height : 0;
	int32_t inner_w = width - 2 * border;
	int32_t inner_h = height - 2 * border;
	int32_t avail_w = bar_decoration_avail_width(node);
	int32_t avail_h = inner_h - 2 * node->padding_y;

	if (avail_w <= 0 || avail_h <= 0) {
		text_node_clear(node->scene_buffer, &node->buffer, node->border,
						node->bg);
		node->cached_scale = -1.0f;
		node->cached_clip_pixel_w = -1;
		node->text_logical_w = 0;
		node->text_logical_h = 0;
		node->logical_width = 0;
		node->logical_height = 0;
		bar_buttons_sync(node);
		return;
	}

	/* Contents are laid out against the bar's full size so they slide with the
	 * window; only the visible part is painted. */
	int32_t box_x = 0, box_y = 0, box_w = width, box_h = height;
	if (node->visible.width > 0 && node->visible.height > 0) {
		box_x = node->visible.x;
		box_y = node->visible.y;
		box_w = node->visible.width;
		box_h = node->visible.height;
	}
	inner_w = box_w - 2 * border;
	inner_h = box_h - 2 * border;
	if (inner_w < 0)
		inner_w = 0;
	if (inner_h < 0)
		inner_h = 0;

	struct fx_corner_radii outer = bar_outer_corners(node);
	/* A side that has slid off the monitor has no corner left to round. */
	if (box_x > 0) {
		outer.top_left = 0;
		outer.bottom_left = 0;
	}
	if (box_y > 0) {
		outer.top_left = 0;
		outer.top_right = 0;
	}
	if (box_x + box_w < width) {
		outer.top_right = 0;
		outer.bottom_right = 0;
	}
	if (box_y + box_h < height) {
		outer.bottom_left = 0;
		outer.bottom_right = 0;
	}
	struct fx_corner_radii inner = inner_corners_of(outer, border);

	const float *bg_color =
		node->focused ? node->focus_bg_color : node->bg_color;
	float faded_border[4];
	float faded_bg[4];
	bar_faded_color(node, node->border_color, faded_border);
	bar_faded_color(node, bg_color, faded_bg);

	if (border > 0) {
		rect_apply_border(node->border, faded_border, outer, inner, border,
						  box_x, box_y, box_w, box_h);
		rect_apply(node->bg, faded_bg, inner, box_x + border, box_y + border,
				   inner_w, inner_h);
	} else {
		/* No border rect: the window frame's border surrounds the bar. */
		wlr_scene_rect_set_size(node->border, 0, 0);
		rect_apply(node->bg, faded_bg, outer, box_x, box_y, box_w, box_h);
	}
#if TEXT_NODE_CORNER_RADIUS
	/* Only the outermost rect may snap; snapping an inset one would push it
	 * over its border. */
	if (border > 0)
		wlr_scene_rect_set_snap_edges(node->border, node->snap_edges);
	wlr_scene_rect_set_snap_edges(node->bg, border == 0 ? node->snap_edges
														: WLR_EDGE_NONE);
#endif

	node->logical_width = width;
	node->logical_height = height;

	float scale = node->cached_scale > 0.0f ? node->cached_scale : 1.0f;
	text_node_apply_size(node->scene_buffer, node->surface_pixel_w,
						 node->surface_pixel_h, scale, 0, avail_h,
						 &node->text_logical_w, &node->text_logical_h);

	int32_t offset_x = (avail_w - node->text_logical_w) / 2;
	int32_t offset_y = (avail_h - node->text_logical_h) / 2;
	if (offset_x < 0) {
		offset_x = 0;
	}
	if (offset_y < 0) {
		offset_y = 0;
	}

	int32_t text_x = bar_decoration_side_inset(node) + offset_x;

	wlr_scene_node_set_position(&node->scene_buffer->node, text_x,
								border + node->padding_y + offset_y);
	bar_clip_buffer(node, node->scene_buffer);

	bar_buttons_sync(node);
}

void mango_bar_decoration_set_size(MangoBarDecoration *node, int32_t width,
								   int32_t height) {
	if (!node)
		return;

	if (width < 0)
		width = 0;
	if (height < 0)
		height = 0;

	if (node->target_width == width && node->target_height == height &&
		node->cached_text)
		return;

	node->target_width = width;
	node->target_height = height;

	const char *redraw_text = node->last_text ? node->last_text : "";
	float redraw_scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;

	mango_bar_decoration_update(node, redraw_text, redraw_scale);
}

static bool close_button_box(const MangoBarDecoration *node, int32_t *out_x,
							 int32_t *out_y, int32_t *out_size) {
	if (!node->close_enabled || node->close_size <= 0 ||
		node->target_width <= 0 || node->target_height <= 0)
		return false;

	int32_t size = node->close_size;
	if (size > node->target_height)
		size = node->target_height;
	if (size > node->target_width)
		size = node->target_width;
	int32_t margin = node->close_margin;
	if (margin > node->target_width - size)
		margin = node->target_width - size;

	*out_size = size;
	*out_x = node->target_width - size - margin;
	*out_y = (node->target_height - size) / 2;
	return true;
}

static void close_button_render(MangoBarDecoration *node, int32_t size,
								int32_t pixel, float scale, bool hover) {
	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, pixel, pixel);
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surface);
		text_node_install(node->close_buffer, &node->close_texture, NULL, 0, 0);
		return;
	}

	cairo_t *cr = cairo_create(surface);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	cairo_set_source_rgba(cr, 0.0, 0.0, 0.0, 0.0);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

	double center = pixel / 2.0;
	const float *circle = hover ? node->close_hover_color : node->close_color;
	cairo_set_source_rgba(cr, circle[0], circle[1], circle[2], circle[3]);
	cairo_arc(cr, center, center, center, 0, 2 * 3.14159265358979323846);
	cairo_fill(cr);

	double inset = pixel * 0.30;
	double line_width = pixel * 0.12;
	if (line_width < 1.0)
		line_width = 1.0;
	cairo_set_line_width(cr, line_width);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_set_source_rgba(cr, node->close_x_color[0], node->close_x_color[1],
						  node->close_x_color[2], node->close_x_color[3]);
	cairo_move_to(cr, inset, inset);
	cairo_line_to(cr, pixel - inset, pixel - inset);
	cairo_move_to(cr, pixel - inset, inset);
	cairo_line_to(cr, inset, pixel - inset);
	cairo_stroke(cr);

	cairo_surface_flush(surface);
	cairo_destroy(cr);

	text_node_install(node->close_buffer, &node->close_texture, surface, pixel,
					  pixel);

	node->close_cached_size = size;
	node->close_cached_scale = scale;
	node->close_cached_hover = hover;
	node->close_cached_valid = true;
}

static void close_button_sync(MangoBarDecoration *node) {
	if (!node || !node->close_buffer)
		return;

	int32_t x = 0, y = 0, size = 0;
	if (!close_button_box(node, &x, &y, &size)) {
		text_node_install(node->close_buffer, &node->close_texture, NULL, 0, 0);
		node->close_cached_valid = false;
		wlr_scene_node_set_enabled(&node->close_buffer->node, false);
		return;
	}

	float scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;
	int32_t pixel = (int32_t)(size * scale + 0.5f);
	if (pixel < 1)
		pixel = 1;

	if (!node->close_cached_valid || node->close_cached_size != size ||
		node->close_cached_scale != scale ||
		node->close_cached_hover != node->close_hover) {
		close_button_render(node, size, pixel, scale, node->close_hover);
	}

	wlr_scene_buffer_set_dest_size(node->close_buffer, size, size);
	wlr_scene_node_set_position(&node->close_buffer->node, x, y);
	bar_clip_buffer(node, node->close_buffer);
	wlr_scene_node_place_above(&node->close_buffer->node,
							   &node->scene_buffer->node);
	wlr_scene_node_set_enabled(&node->close_buffer->node, true);
}

void mango_bar_decoration_update(MangoBarDecoration *node, const char *text,
								 float scale) {
	if (!node || !text)
		return;
	if (scale <= 0.0f) {
		scale = 1.0f;
	}

	if (!node->last_text || strcmp(node->last_text, text) != 0) {
		g_free(node->last_text);
		node->last_text = g_strdup(text);
	}
	node->last_scale = scale;

	/* While hidden, only keep the pending text current (no rasterizing):
	 * invalidating `cached_text` makes the next visible render use the final
	 * geometry, not a zero-width one. */
	if (!node->scene->node.enabled) {
		g_free(node->cached_text);
		node->cached_text = NULL;
		node->cached_scale = -1.0f;
		return;
	}

	const float *fg_color =
		node->focused ? node->focus_fg_color : node->fg_color;

	int32_t avail_w = bar_decoration_avail_width(node);
	int32_t avail_pixel_w = (int32_t)(avail_w * scale + 0.5f);

	int32_t natural_pixel_w = 0, natural_pixel_h = 0;
	measure_text(&node->measure, node->font_desc, text, scale, &natural_pixel_w,
				 &natural_pixel_h);

	bool overlong = avail_pixel_w > 0 && natural_pixel_w > avail_pixel_w;
	int32_t clip_pixel_w = overlong ? avail_pixel_w : 0;

	bool dirty =
		node->cached_scale != scale || !node->cached_text ||
		strcmp(node->cached_text, text) != 0 || !node->cached_font_desc ||
		strcmp(node->cached_font_desc, node->font_desc) != 0 ||
		memcmp(node->cached_fg_color, fg_color, sizeof(node->fg_color)) != 0 ||
		node->cached_focused != node->focused ||
		node->cached_center_secondary_line != node->center_secondary_line ||
		node->cached_clip_pixel_w != clip_pixel_w;

	if (dirty) {
		char *ellipsized = NULL;
		const char *display = text;
		if (overlong) {
			ellipsized = text_ellipsize(&node->measure, node->font_desc, text,
										scale, avail_pixel_w);
			display = ellipsized;
		}

		g_free(node->cached_text);
		node->cached_text = g_strdup(text);
		g_free(node->cached_font_desc);
		node->cached_font_desc = g_strdup(node->font_desc);
		node->cached_scale = scale;
		memcpy(node->cached_fg_color, fg_color, sizeof(node->cached_fg_color));
		node->cached_focused = node->focused;
		node->cached_center_secondary_line = node->center_secondary_line;
		node->cached_clip_pixel_w = clip_pixel_w;

		cairo_surface_t *surface = NULL;
		const char *second_line =
			node->center_secondary_line ? strchr(display, '\n') : NULL;
		if (second_line) {
			char *first = g_strndup(display, second_line - display);
			surface = text_surface_create_two_lines(
				&node->measure, node->font_desc, first, second_line + 1, scale,
				fg_color, &node->surface_pixel_w, &node->surface_pixel_h);
			g_free(first);
		} else {
			surface = text_surface_create(
				&node->measure, node->font_desc, display, scale, fg_color,
				&node->surface_pixel_w, &node->surface_pixel_h);
		}
		text_node_install(node->scene_buffer, &node->buffer, surface,
						  node->surface_pixel_w, node->surface_pixel_h);
		g_free(ellipsized);
	}

	bar_decoration_apply_geometry(node);
	close_button_sync(node);
}

void mango_bar_decoration_set_focus(MangoBarDecoration *node, bool focused) {
	if (!node || node->focused == focused)
		return;
	node->focused = focused;
	if (node->last_text) {
		float scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;
		mango_bar_decoration_update(node, node->last_text, scale);
	}
}

void mango_bar_decoration_set_center_secondary_line(MangoBarDecoration *node,
													bool centered) {
	if (!node || node->center_secondary_line == centered)
		return;

	node->center_secondary_line = centered;
	if (node->last_text) {
		float scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;
		mango_bar_decoration_update(node, node->last_text, scale);
	}
}

void mango_bar_decoration_set_colors(MangoBarDecoration *node,
									 const float fg[4], const float bg[4]) {
	if (!node)
		return;

	memcpy(node->fg_color, fg, sizeof(node->fg_color));
	memcpy(node->bg_color, bg, sizeof(node->bg_color));

	if (node->last_text) {
		float scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;
		mango_bar_decoration_update(node, node->last_text, scale);
	}
}

void mango_bar_decoration_set_close(MangoBarDecoration *node, bool enabled,
									int32_t size, int32_t margin) {
	if (!node)
		return;

	if (node->close_enabled == enabled && node->close_size == size &&
		node->close_margin == margin)
		return;

	node->close_enabled = enabled;
	node->close_size = size;
	node->close_margin = margin;
	close_button_sync(node);
}

void mango_bar_decoration_set_close_color(MangoBarDecoration *node,
										  const float circle[4]) {
	if (!node || !circle)
		return;

	memcpy(node->close_color, circle, sizeof(node->close_color));
	for (int i = 0; i < 3; i++)
		node->close_hover_color[i] = circle[i] + (1.0f - circle[i]) * 0.4f;
	node->close_hover_color[3] = circle[3];

	node->close_cached_valid = false;
	close_button_sync(node);
}

void mango_bar_decoration_set_close_hover(MangoBarDecoration *node,
										  bool hover) {
	if (!node || node->close_hover == hover)
		return;
	node->close_hover = hover;
	close_button_sync(node);
}

bool mango_bar_decoration_close_contains(MangoBarDecoration *node, double lx,
										 double ly) {
	if (!node)
		return false;

	int32_t x = 0, y = 0, size = 0;
	if (!close_button_box(node, &x, &y, &size))
		return false;

	return lx >= x && lx < x + size && ly >= y && ly < y + size;
}

void mango_bar_decoration_set_buttons(MangoBarDecoration *node, bool enabled,
									  bool left, bool inverse, int32_t size,
									  int32_t gap, int32_t margin) {
	if (!node)
		return;
	if (node->buttons_enabled == enabled && node->buttons_left == left &&
		node->buttons_inverse == inverse && node->button_size == size &&
		node->button_gap == gap && node->button_margin == margin)
		return;

	node->buttons_enabled = enabled;
	node->buttons_left = left;
	node->buttons_inverse = inverse;
	node->button_size = size;
	node->button_gap = gap;
	node->button_margin = margin;

	if (!enabled) {
		node->hover_button = -1;
		node->buttons_hover = false;
	}

	if (node->last_text) {
		float scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;
		mango_bar_decoration_update(node, node->last_text, scale);
	} else {
		bar_buttons_sync(node);
	}
}

void mango_bar_decoration_set_button_color(MangoBarDecoration *node,
										   int32_t button,
										   const float circle[4]) {
	if (!node || !circle || button < 0 || button >= MANGO_BAR_BUTTON_COUNT)
		return;

	MangoBarButton *target = &node->buttons[button];
	memcpy(target->color, circle, sizeof(target->color));
	for (int i = 0; i < 3; i++)
		target->hover_color[i] = circle[i] + (1.0f - circle[i]) * 0.4f;
	target->hover_color[3] = circle[3];

	target->cached_valid = false;
	bar_buttons_sync(node);
}

void mango_bar_decoration_set_button_hover(MangoBarDecoration *node,
										   int32_t button, bool hover) {
	if (!node)
		return;

	int32_t next = hover ? button : -1;
	if (node->hover_button == next)
		return;

	node->hover_button = next;
	bar_buttons_sync(node);
}
int32_t mango_bar_decoration_button_at(MangoBarDecoration *node, double lx,
									   double ly) {
	if (!node)
		return -1;

	for (int32_t i = 0; i < MANGO_BAR_BUTTON_COUNT; i++) {
		int32_t x = 0, y = 0, size = 0;
		if (!bar_button_box(node, i, &x, &y, &size))
			continue;
		if (lx >= x && lx < x + size && ly >= y && ly < y + size)
			return i;
	}
	return -1;
}

/* Footprint of the whole strip, gaps included: the union of every button box.
 * Index 0 is not necessarily the leftmost, so take a min/max over all of them.
 */
static bool bar_buttons_bounds(const MangoBarDecoration *node, int32_t *out_x0,
							   int32_t *out_y0, int32_t *out_x1,
							   int32_t *out_y1) {
	bool any = false;
	int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;

	for (int32_t i = 0; i < MANGO_BAR_BUTTON_COUNT; i++) {
		int32_t x = 0, y = 0, size = 0;
		if (!bar_button_box(node, i, &x, &y, &size))
			continue;

		if (!any) {
			x0 = x;
			x1 = x + size;
			y0 = y;
			y1 = y + size;
			any = true;
			continue;
		}
		if (x < x0)
			x0 = x;
		if (x + size > x1)
			x1 = x + size;
		if (y < y0)
			y0 = y;
		if (y + size > y1)
			y1 = y + size;
	}

	if (!any)
		return false;

	*out_x0 = x0;
	*out_y0 = y0;
	*out_x1 = x1;
	*out_y1 = y1;
	return true;
}

bool mango_bar_decoration_buttons_contains(MangoBarDecoration *node, double lx,
										   double ly) {
	if (!node)
		return false;

	int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	if (!bar_buttons_bounds(node, &x0, &y0, &x1, &y1))
		return false;

	return lx >= x0 && lx < x1 && ly >= y0 && ly < y1;
}

void mango_bar_decoration_set_buttons_hover(MangoBarDecoration *node,
											bool hover) {
	if (!node || node->buttons_hover == hover)
		return;

	node->buttons_hover = hover;
	bar_buttons_sync(node);
}

void mango_jump_label_node_apply_config(MangoJumpLabel *node,
										const DecorateDrawData *data) {
	if (!node || !data)
		return;

	memcpy(node->fg_color, data->fg_color, sizeof(node->fg_color));
	memcpy(node->bg_color, data->bg_color, sizeof(node->bg_color));
	memcpy(node->focus_fg_color, data->focus_fg_color,
		   sizeof(node->focus_fg_color));
	memcpy(node->focus_bg_color, data->focus_bg_color,
		   sizeof(node->focus_bg_color));
	memcpy(node->border_color, data->border_color, sizeof(node->border_color));
	node->border_width = data->border_width;
	node->corner_radius = data->corner_radius;
	node->padding_x = data->padding_x;
	node->padding_y = data->padding_y;

	g_free(node->font_desc);
	node->font_desc =
		g_strdup(data->font_desc ? data->font_desc : "monospace Bold 16");

	if (node->cached_text) {
		mango_jump_label_node_update(
			node, node->cached_text,
			node->cached_scale > 0.0f ? node->cached_scale : 1.0f);
	}
}

void mango_bar_decoration_apply_config(MangoBarDecoration *node,
									   const DecorateDrawData *data) {
	if (!node || !data)
		return;

	memcpy(node->fg_color, data->fg_color, sizeof(node->fg_color));
	memcpy(node->bg_color, data->bg_color, sizeof(node->bg_color));
	memcpy(node->focus_fg_color, data->focus_fg_color,
		   sizeof(node->focus_fg_color));
	memcpy(node->focus_bg_color, data->focus_bg_color,
		   sizeof(node->focus_bg_color));
	memcpy(node->border_color, data->border_color, sizeof(node->border_color));
	node->border_width = data->border_width;
	node->corner_radius = data->corner_radius;
	node->padding_x = data->padding_x;
	node->padding_y = data->padding_y;

	g_free(node->font_desc);
	node->font_desc =
		g_strdup(data->font_desc ? data->font_desc : "monospace Bold 13");

	if (node->last_text) {
		float scale = node->last_scale > 0.0f ? node->last_scale : 1.0f;
		mango_bar_decoration_update(node, node->last_text, scale);
	}
}
