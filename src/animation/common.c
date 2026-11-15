#include "mango/animation/common.h"
#include "mango/common/scene_node.h"
#include "mango/common/server.h"
#include "mango/common/util.h"
#include "mango/config/parse.h"
#include "mango/manage/client.h"
#include "mango/manage/monitor.h"
#include <math.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_subcompositor.h>

struct dvec2 calculate_animation_curve_at(double t, int32_t type) {
	struct dvec2 point;
	double *animation_curve;
	if (type == MOVE) {
		animation_curve = config.animation_curve_move;
	} else if (type == OPEN) {
		animation_curve = config.animation_curve_open;
	} else if (type == TAG) {
		animation_curve = config.animation_curve_tag;
	} else if (type == CLOSE) {
		animation_curve = config.animation_curve_close;
	} else if (type == FOCUS) {
		animation_curve = config.animation_curve_focus;
	} else if (type == OPAFADEIN) {
		animation_curve = config.animation_curve_opafadein;
	} else if (type == OPAFADEOUT) {
		animation_curve = config.animation_curve_opafadeout;
	} else {
		animation_curve = config.animation_curve_move;
	}

	point.x = 3 * t * (1 - t) * (1 - t) * animation_curve[0] +
			  3 * t * t * (1 - t) * animation_curve[2] + t * t * t;

	point.y = 3 * t * (1 - t) * (1 - t) * animation_curve[1] +
			  3 * t * t * (1 - t) * animation_curve[3] + t * t * t;

	return point;
}
void handle_snapshot_meta_destroy(struct wl_listener *listener, void *data) {
	SnapshotMetadata *meta = wl_container_of(listener, meta, destroy);
	wl_list_remove(&meta->destroy.link);
	free(meta);
}
void init_baked_points(void) {
	server.baked_points_move =
		calloc(BAKED_POINTS_COUNT, sizeof(*server.baked_points_move));
	server.baked_points_open =
		calloc(BAKED_POINTS_COUNT, sizeof(*server.baked_points_open));
	server.baked_points_tag =
		calloc(BAKED_POINTS_COUNT, sizeof(*server.baked_points_tag));
	server.baked_points_close =
		calloc(BAKED_POINTS_COUNT, sizeof(*server.baked_points_close));
	server.baked_points_focus =
		calloc(BAKED_POINTS_COUNT, sizeof(*server.baked_points_focus));
	server.baked_points_opafadein =
		calloc(BAKED_POINTS_COUNT, sizeof(*server.baked_points_opafadein));
	server.baked_points_opafadeout =
		calloc(BAKED_POINTS_COUNT, sizeof(*server.baked_points_opafadeout));

	for (int32_t i = 0; i < BAKED_POINTS_COUNT; i++) {
		server.baked_points_move[i] = calculate_animation_curve_at(
			(double)i / (BAKED_POINTS_COUNT - 1), MOVE);
	}
	for (int32_t i = 0; i < BAKED_POINTS_COUNT; i++) {
		server.baked_points_open[i] = calculate_animation_curve_at(
			(double)i / (BAKED_POINTS_COUNT - 1), OPEN);
	}
	for (int32_t i = 0; i < BAKED_POINTS_COUNT; i++) {
		server.baked_points_tag[i] = calculate_animation_curve_at(
			(double)i / (BAKED_POINTS_COUNT - 1), TAG);
	}
	for (int32_t i = 0; i < BAKED_POINTS_COUNT; i++) {
		server.baked_points_close[i] = calculate_animation_curve_at(
			(double)i / (BAKED_POINTS_COUNT - 1), CLOSE);
	}
	for (int32_t i = 0; i < BAKED_POINTS_COUNT; i++) {
		server.baked_points_focus[i] = calculate_animation_curve_at(
			(double)i / (BAKED_POINTS_COUNT - 1), FOCUS);
	}
	for (int32_t i = 0; i < BAKED_POINTS_COUNT; i++) {
		server.baked_points_opafadein[i] = calculate_animation_curve_at(
			(double)i / (BAKED_POINTS_COUNT - 1), OPAFADEIN);
	}
	for (int32_t i = 0; i < BAKED_POINTS_COUNT; i++) {
		server.baked_points_opafadeout[i] = calculate_animation_curve_at(
			(double)i / (BAKED_POINTS_COUNT - 1), OPAFADEOUT);
	}
}
double find_animation_curve_at(double t, int32_t type) {
	int32_t down = 0;
	int32_t up = BAKED_POINTS_COUNT - 1;

	int32_t middle = (up + down) / 2;
	struct dvec2 *baked_points;
	if (type == MOVE) {
		baked_points = server.baked_points_move;
	} else if (type == OPEN) {
		baked_points = server.baked_points_open;
	} else if (type == TAG) {
		baked_points = server.baked_points_tag;
	} else if (type == CLOSE) {
		baked_points = server.baked_points_close;
	} else if (type == FOCUS) {
		baked_points = server.baked_points_focus;
	} else if (type == OPAFADEIN) {
		baked_points = server.baked_points_opafadein;
	} else if (type == OPAFADEOUT) {
		baked_points = server.baked_points_opafadeout;
	} else {
		baked_points = server.baked_points_move;
	}

	while (up - down != 1) {
		if (baked_points[middle].x <= t) {
			down = middle;
		} else {
			up = middle;
		}
		middle = (up + down) / 2;
	}
	return baked_points[up].y;
}
bool scene_node_snapshot(struct wlr_scene_node *node, int32_t lx, int32_t ly,
						 struct wlr_scene_tree *snapshot_tree) {
	if (!node->enabled && node->type != WLR_SCENE_NODE_TREE) {
		return true;
	}

	lx += node->x;
	ly += node->y;

	struct wlr_scene_node *snapshot_node = NULL;
	switch (node->type) {
	case WLR_SCENE_NODE_TREE: {
		struct wlr_scene_tree *scene_tree = wlr_scene_tree_from_node(node);

		struct wlr_scene_node *child;
		wl_list_for_each(child, &scene_tree->children, link) {
			scene_node_snapshot(child, lx, ly, snapshot_tree);
		}
		break;
	}
	case WLR_SCENE_NODE_RECT: {
		struct wlr_scene_rect *scene_rect = wlr_scene_rect_from_node(node);

		/* Rect nodes carry most of what a window is decorated with: the
		 * border ring and the title bar's background. Leaving them out of
		 * the snapshot made them vanish the instant a close animation
		 * started, while the surface, the title and the traffic lights
		 * faded out normally - the frame and its bar were simply gone and
		 * only the buffer survived. */
		struct wlr_scene_rect *snapshot_rect =
			wlr_scene_rect_create(snapshot_tree, scene_rect->width,
								  scene_rect->height, scene_rect->color);
		if (snapshot_rect == NULL) {
			return false;
		}

		/* The colour arrives premultiplied and wlr_scene_rect_create() takes
		 * it as-is, so it is copied verbatim rather than through
		 * scene_rect_set_color(), which would premultiply a second time. */
		wlr_scene_rect_set_corner_radii(snapshot_rect, scene_rect->corners);
		wlr_scene_rect_set_clipped_region(snapshot_rect,
										  scene_rect->clipped_region);
		wlr_scene_rect_set_snap_edges(snapshot_rect, scene_rect->snap_edges);

		/* Rect nodes have no opacity of their own, so a fade scales their
		 * colour. Keep the colour it started from: scaling the faded result
		 * every tick would drive the bar and the ring to black instead of
		 * letting them fade out. */
		SnapshotMetadata *meta = calloc(1, sizeof(*meta));
		if (meta == NULL) {
			wlr_scene_node_destroy(&snapshot_rect->node);
			return false;
		}
		meta->orig_width = scene_rect->width;
		meta->orig_height = scene_rect->height;
		/* The snapshot rect is positioned at (lx, ly), so these are the box
		 * origin a zoom has to scale from. */
		meta->orig_x = lx;
		meta->orig_y = ly;
		memcpy(meta->orig_color, scene_rect->color, sizeof(meta->orig_color));
		meta->orig_corners = scene_rect->corners;
		meta->orig_clipped_region = scene_rect->clipped_region;
		meta->type = Snapshot;
		meta->destroy.notify = handle_snapshot_meta_destroy;
		wl_signal_add(&snapshot_rect->node.events.destroy, &meta->destroy);
		mango_scene_node_set(&snapshot_rect->node, Snapshot, meta);

		snapshot_node = &snapshot_rect->node;
		break;
	}
	case WLR_SCENE_NODE_BUFFER: {
		struct wlr_scene_buffer *scene_buffer =
			wlr_scene_buffer_from_node(node);

		// Creates an intermediate wrapper tree node.
		struct wlr_scene_tree *wrapper = wlr_scene_tree_create(snapshot_tree);
		if (wrapper == NULL) {
			return false;
		}
		snapshot_node = &wrapper->node; // Applies the coordinate offset to the
										// outer wrapper box.

		// Collects surface state and stores it as metadata.
		SnapshotMetadata *meta = calloc(1, sizeof(SnapshotMetadata));
		if (meta == NULL) {
			wlr_scene_node_destroy(&wrapper->node);
			return false;
		}
		meta->orig_width = scene_buffer->dst_width;
		meta->orig_height = scene_buffer->dst_height;
		meta->orig_src_box = scene_buffer->src_box;
		/* The wrapper carries the buffer's absolute place in the snapshot, and
		 * the buffer itself sits at its origin. Recording the wrapper's
		 * position here is what lets a zoom move it with the frame; the buffer
		 * keeps whatever offset the clipping needs on top of that. */
		meta->orig_x = lx;
		meta->orig_y = ly;
		/* The buffer's own rounding has to shrink with it: it is the bottom
		 * corners of a title-barred window, which are all that is left of the
		 * frame's curve once the bar covers the top. Left unscaled they keep
		 * their full radius while the frame shrinks, and a wedge of the frame's
		 * interior opens between the border and the surface. */
		meta->orig_corners = scene_buffer->corners;
		meta->orig_clipped_region = scene_buffer->clipped_region;
		meta->type = Snapshot;

		struct wlr_scene_surface *scene_surface =
			wlr_scene_surface_try_from_buffer(scene_buffer);
		if (scene_surface != NULL) {
			meta->is_subsurface =
				!!wlr_subsurface_try_from_wlr_surface(scene_surface->surface);
		}

		// bind a destruction callback listener to free memory when the wrapper
		// node is destroyed
		meta->destroy.notify = handle_snapshot_meta_destroy;
		wl_signal_add(&wrapper->node.events.destroy, &meta->destroy);
		mango_scene_node_set(&wrapper->node, Snapshot, meta);

		// attach the real buffer underneath the wrapper (relative coordinates
		// 0,0)
		struct wlr_scene_buffer *snapshot_buffer =
			wlr_scene_buffer_create(wrapper, NULL);
		if (snapshot_buffer == NULL) {
			wlr_scene_node_destroy(&wrapper->node);
			return false;
		}

		/*
		 * Do not copy the source node's payload onto the snapshot buffer. A
		 * MangoSceneNode is owned by (and freed together with) its original
		 * node, so aliasing it here would dangle once that node is destroyed
		 * and would make an ancestor lookup resolve to the wrong root. The
		 * wrapping tree already carries its own Snapshot payload, so walking
		 * up from this buffer still resolves correctly.
		 */

		wlr_scene_buffer_set_dest_size(snapshot_buffer, scene_buffer->dst_width,
									   scene_buffer->dst_height);
		wlr_scene_buffer_set_opaque_region(snapshot_buffer,
										   &scene_buffer->opaque_region);
		wlr_scene_buffer_set_source_box(snapshot_buffer,
										&scene_buffer->src_box);
		wlr_scene_buffer_set_transform(snapshot_buffer,
									   scene_buffer->transform);
		wlr_scene_buffer_set_filter_mode(snapshot_buffer,
										 scene_buffer->filter_mode);

		// Effects
		wlr_scene_buffer_set_opacity(snapshot_buffer, scene_buffer->opacity);
		/* The corners and the clip are re-applied from the recorded originals
		 * on every tick of a zoom, since both have to shrink with the buffer.
		 * A clipped surface must stay clipped while it is animated, otherwise
		 * the snapshot's corners pop into the frame's arc. */
		wlr_scene_buffer_set_corner_radii(snapshot_buffer,
										  scene_buffer->corners);
		wlr_scene_buffer_set_clipped_region(snapshot_buffer,
											scene_buffer->clipped_region);

		if (scene_surface != NULL && scene_surface->surface->buffer != NULL) {
			wlr_scene_buffer_set_buffer(snapshot_buffer,
										&scene_surface->surface->buffer->base);
		} else {
			wlr_scene_buffer_set_buffer(snapshot_buffer, scene_buffer->buffer);
		}
		break;
	}
	case WLR_SCENE_NODE_SHADOW: {
		struct wlr_scene_shadow *scene_shadow =
			wlr_scene_shadow_from_node(node);

		struct wlr_scene_shadow *snapshot_shadow = wlr_scene_shadow_create(
			snapshot_tree, scene_shadow->width, scene_shadow->height,
			scene_shadow->corner_radius, scene_shadow->blur_sigma,
			scene_shadow->color);
		if (snapshot_shadow == NULL) {
			return false;
		}
		snapshot_node = &snapshot_shadow->node;

		wlr_scene_shadow_set_clipped_region(snapshot_shadow,
											scene_shadow->clipped_region);
		wlr_scene_shadow_set_corner_exponent(snapshot_shadow,
											 scene_shadow->corner_exponent);

		wlr_scene_node_set_enabled(&snapshot_shadow->node, false);

		break;
	}
	case WLR_SCENE_NODE_BLUR:
		break;
	case WLR_SCENE_NODE_OPTIMIZED_BLUR:
		return true;
	}

	if (snapshot_node != NULL) {
		wlr_scene_node_set_position(snapshot_node, lx, ly);
	}

	return true;
}
struct wlr_scene_tree *wlr_scene_tree_snapshot(struct wlr_scene_node *node,
											   struct wlr_scene_tree *parent) {
	struct wlr_scene_tree *snapshot = wlr_scene_tree_create(parent);
	if (snapshot == NULL) {
		return NULL;
	}

	// Disable and enable the snapshot tree like so to atomically update
	// the scene-graph. This will prevent over-damaging or other weirdness.
	wlr_scene_node_set_enabled(&snapshot->node, false);

	if (!scene_node_snapshot(node, 0, 0, snapshot)) {
		wlr_scene_node_destroy(&snapshot->node);
		return NULL;
	}

	wlr_scene_node_set_enabled(&snapshot->node, true);

	return snapshot;
}

static void snapshot_dest_to_source(enum wl_output_transform transform, float x,
									float y, float *src_x, float *src_y) {
	switch (transform) {
	case WL_OUTPUT_TRANSFORM_90:
		*src_x = 1.0f - y;
		*src_y = x;
		break;
	case WL_OUTPUT_TRANSFORM_180:
		*src_x = 1.0f - x;
		*src_y = 1.0f - y;
		break;
	case WL_OUTPUT_TRANSFORM_270:
		*src_x = y;
		*src_y = 1.0f - x;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED:
		*src_x = 1.0f - x;
		*src_y = y;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_90:
		*src_x = y;
		*src_y = x;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_180:
		*src_x = x;
		*src_y = 1.0f - y;
		break;
	case WL_OUTPUT_TRANSFORM_FLIPPED_270:
		*src_x = 1.0f - y;
		*src_y = 1.0f - x;
		break;
	case WL_OUTPUT_TRANSFORM_NORMAL:
	default:
		*src_x = x;
		*src_y = y;
		break;
	}
}

struct SnapshotClipData {
	struct wlr_box bounds;
	int32_t width;
	int32_t height;
	bool scale;
	/* The frame the snapshot was taken at, in the same absolute coordinates
	 * the snapshot's children use. A zoom shrinks the frame's buffers by
	 * rewriting their destination size, but a rect is its box: it has to be
	 * shrunk by rewriting the box, so everything is mapped from this frame into
	 * the animated one. The frame's origin matters as much as its size, since
	 * it is the point the shrink is centred on. */
	struct wlr_box frame;
};

static void scene_snapshot_enable_buffers(struct wlr_scene_node *node) {
	if (node->type == WLR_SCENE_NODE_BUFFER) {
		if (!node->enabled) {
			wlr_scene_node_set_enabled(node, true);
		}
		return;
	}
	if (node->type != WLR_SCENE_NODE_TREE) {
		return;
	}

	struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
	struct wlr_scene_node *child;
	wl_list_for_each(child, &tree->children, link) {
		scene_snapshot_enable_buffers(child);
	}
}

static struct fx_corner_radii
snapshot_corners_scale(struct fx_corner_radii corners, double scale) {
	corners.top_left = (uint16_t)(corners.top_left * scale);
	corners.top_right = (uint16_t)(corners.top_right * scale);
	corners.bottom_right = (uint16_t)(corners.bottom_right * scale);
	corners.bottom_left = (uint16_t)(corners.bottom_left * scale);
	return corners;
}

/* The hole a buffer cuts into itself (the window frame's inner arc) has to
 * shrink with the buffer for the same reason its corners do. */
static struct clipped_region snapshot_hole_scale(struct clipped_region hole,
												 double scale_x, double scale_y,
												 double scale) {
	if (hole.area.width <= 0 || hole.area.height <= 0) {
		return hole;
	}
	hole.area.x = (int32_t)(hole.area.x * scale_x);
	hole.area.y = (int32_t)(hole.area.y * scale_y);
	hole.area.width = (int32_t)(hole.area.width * scale_x);
	hole.area.height = (int32_t)(hole.area.height * scale_y);
	hole.corners = snapshot_corners_scale(hole.corners, scale);
	return hole;
}

static void scene_snapshot_clip_buffer(struct wlr_scene_buffer *buffer,
									   int32_t sx, int32_t sy, void *data) {
	struct SnapshotClipData *clip = data;
	MangoSceneNode *snapshot_node = mango_scene_node_find(&buffer->node);
	if (!snapshot_node || snapshot_node->type != Snapshot ||
		!snapshot_node->owner) {
		return;
	}

	SnapshotMetadata *meta = snapshot_node->owner;
	/* sx/sy are the buffer's place in the snapshot already: the wrapper node
	 * holds meta->orig_x/orig_y and the buffer only holds the offset the
	 * clipping gave it last tick, which is not part of the original layout. */
	int32_t base_x = sx;
	int32_t base_y = sy;
	int32_t dest_width = meta->orig_width;
	int32_t dest_height = meta->orig_height;

	/* A zoom scales the whole frame, so every buffer shrinks by the frame's
	 * ratio, not to the frame's size: a window also holds the title text and
	 * the traffic lights, and stretching each of them to the frame box made the
	 * surface fill the bar's rows and spill past the border's bottom edge. */
	if (clip->scale && clip->frame.width > 0 && clip->frame.height > 0) {
		double scale_x = (double)clip->width / (double)clip->frame.width;
		double scale_y = (double)clip->height / (double)clip->frame.height;
		double scale = MANGO_MIN(scale_x, scale_y);

		dest_width = (int32_t)((int64_t)meta->orig_width * clip->width /
							   clip->frame.width);
		dest_height = (int32_t)((int64_t)meta->orig_height * clip->height /
								clip->frame.height);

		/* The window surface's bottom corners and its inner arc are the only
		 * rounding left visible once the title bar covers the top. Copying
		 * them unscaled kept a full-radius arc on a shrinking surface, which
		 * opened a wedge between the border and the content. */
		wlr_scene_buffer_set_corner_radii(
			buffer, snapshot_corners_scale(meta->orig_corners, scale));
		wlr_scene_buffer_set_clipped_region(
			buffer, snapshot_hole_scale(meta->orig_clipped_region, scale_x,
										scale_y, scale));
	}
	if (dest_width <= 0 || dest_height <= 0) {
		dest_width = buffer->WLR_PRIVATE.buffer_width;
		dest_height = buffer->WLR_PRIVATE.buffer_height;
		if (buffer->transform & WL_OUTPUT_TRANSFORM_90) {
			int32_t tmp = dest_width;
			dest_width = dest_height;
			dest_height = tmp;
		}
	}

	wlr_scene_node_set_position(&buffer->node, 0, 0);
	if (dest_width <= 0 || dest_height <= 0) {
		wlr_scene_node_set_enabled(&buffer->node, false);
		return;
	}

	struct wlr_box buffer_box = {
		.x = base_x,
		.y = base_y,
		.width = dest_width,
		.height = dest_height,
	};
	struct wlr_box visible;
	if (!wlr_box_intersection(&visible, &buffer_box, &clip->bounds)) {
		wlr_scene_node_set_enabled(&buffer->node, false);
		return;
	}

	wlr_scene_node_set_enabled(&buffer->node, true);
	int32_t left = visible.x - buffer_box.x;
	int32_t top = visible.y - buffer_box.y;
	wlr_scene_node_set_position(&buffer->node, left, top);
	wlr_scene_buffer_set_dest_size(buffer, visible.width, visible.height);

	/* A side this buffer dropped because it left the monitor has no corner
	 * left to round, the same way the title bar squares off the side that
	 * slid off. Without this, a window sliding out of view rounds off against
	 * the screen edge instead of travelling past it. */
	if (left != 0 || top != 0 || visible.width != dest_width ||
		visible.height != dest_height) {
		struct fx_corner_radii corners = buffer->corners;
		if (top != 0) {
			corners.top_left = 0;
			corners.top_right = 0;
		}
		if (left != 0) {
			corners.top_left = 0;
			corners.bottom_left = 0;
		}
		if (top + visible.height != dest_height) {
			corners.bottom_left = 0;
			corners.bottom_right = 0;
		}
		if (left + visible.width != dest_width) {
			corners.top_right = 0;
			corners.bottom_right = 0;
		}
		wlr_scene_buffer_set_corner_radii(buffer, corners);
	}

	struct wlr_fbox src_box = meta->orig_src_box;
	int32_t src_width = buffer->WLR_PRIVATE.buffer_width;
	int32_t src_height = buffer->WLR_PRIVATE.buffer_height;
	if (!wlr_fbox_empty(&src_box)) {
		src_width = src_box.width;
		src_height = src_box.height;
	} else {
		src_box.x = 0;
		src_box.y = 0;
		src_box.width = src_width;
		src_box.height = src_height;
	}

	if (left == 0 && top == 0 && visible.width == dest_width &&
		visible.height == dest_height) {
		wlr_scene_buffer_set_source_box(
			buffer,
			wlr_fbox_empty(&meta->orig_src_box) ? NULL : &meta->orig_src_box);
		return;
	}

	float nx0 = (float)left / (float)dest_width;
	float ny0 = (float)top / (float)dest_height;
	float nx1 = (float)(left + visible.width) / (float)dest_width;
	float ny1 = (float)(top + visible.height) / (float)dest_height;
	float ax, ay, bx, by;
	snapshot_dest_to_source(buffer->transform, nx0, ny0, &ax, &ay);
	snapshot_dest_to_source(buffer->transform, nx1, ny1, &bx, &by);

	float sx0 = MANGO_MIN(ax, bx);
	float sy0 = MANGO_MIN(ay, by);
	float sx1 = MANGO_MAX(ax, bx);
	float sy1 = MANGO_MAX(ay, by);
	struct wlr_fbox clipped_src = {
		.x = src_box.x + sx0 * src_width,
		.y = src_box.y + sy0 * src_height,
		.width = (sx1 - sx0) * src_width,
		.height = (sy1 - sy0) * src_height,
	};
	wlr_scene_buffer_set_source_box(buffer, &clipped_src);
}

/*
 * Maps a snapshot child's absolute coordinate into the animated frame. A zoom
 * shrinks the frame about its own centre, so the frame's origin is the anchor
 * the children have to be scaled around; using the screen's origin instead
 * pulled every window towards the top left of the layout as it shrank.
 */
static int32_t frame_origin_scale(int32_t frame_origin, int32_t value,
								  double scale) {
	return (int32_t)lround(frame_origin +
						   (double)(value - frame_origin) * scale);
}

/*
 * Maps every snapshot rect from the frame it was taken at into the animated
 * one. Buffers get the same treatment through their destination size, but a
 * rect has no such size to rewrite: its box, its corner radii and the hole it
 * cuts all have to be recomputed. Everything is derived from the geometry
 * recorded at snapshot time, so repeated ticks cannot compound.
 */
static void scene_snapshot_scale_rects(struct wlr_scene_node *node,
									   const struct SnapshotClipData *clip) {
	if (node->type == WLR_SCENE_NODE_RECT) {
		MangoSceneNode *payload = node->data;
		if (!payload || payload->type != Snapshot || !payload->owner) {
			return;
		}

		SnapshotMetadata *meta = payload->owner;
		if (clip->frame.width <= 0 || clip->frame.height <= 0) {
			return;
		}

		double scale_x = (double)clip->width / (double)clip->frame.width;
		double scale_y = (double)clip->height / (double)clip->frame.height;
		/* The corners are a square box in each corner, so they scale by the
		 * smaller of the two factors to stay inside the box. */
		double scale = MANGO_MIN(scale_x, scale_y);

		/* Every snapshot child sits at its snapshot-absolute place directly
		 * under the snapshot tree, and the tree itself only translates: a zoom
		 * never scales the space its children live in. So each child has to
		 * move towards the frame's own origin by the same factor its extent
		 * shrinks by. Anchoring on the screen's origin instead pulled a window
		 * towards the top left of the layout as it shrank. */
		struct wlr_scene_rect *rect = wlr_scene_rect_from_node(node);
		wlr_scene_node_set_position(
			node, frame_origin_scale(clip->frame.x, meta->orig_x, scale_x),
			frame_origin_scale(clip->frame.y, meta->orig_y, scale_y));
		wlr_scene_rect_set_size(rect, (int32_t)(meta->orig_width * scale_x),
								(int32_t)(meta->orig_height * scale_y));

		struct fx_corner_radii corners = meta->orig_corners;
		corners.top_left = (uint16_t)(corners.top_left * scale);
		corners.top_right = (uint16_t)(corners.top_right * scale);
		corners.bottom_right = (uint16_t)(corners.bottom_right * scale);
		corners.bottom_left = (uint16_t)(corners.bottom_left * scale);
		wlr_scene_rect_set_corner_radii(rect, corners);

		/* The hole is what leaves the border visible, and the border is only a
		 * pixel or two wide to start with. Truncating its inset to zero at
		 * any zoom factor below 1 would cut the border away completely on the
		 * sides the hole then reaches, so the inset is rounded and kept at a
		 * pixel wherever the original had one, and the hole is sized from the
		 * shrunk box rather than from its own scaled extent. */
		/* Only the border ring cuts a hole; the title bar's background is a
		 * plain rect, and giving it a hole that covers the whole box would
		 * erase it. The ring's inset is also only a pixel or two to begin
		 * with, so truncating it to zero at any factor below 1 would cut the
		 * border away on those sides: it is rounded and kept at a pixel
		 * wherever the original had one, and the hole is sized from the shrunk
		 * box rather than from its own scaled extent. */
		struct clipped_region hole = meta->orig_clipped_region;
		if (hole.area.width > 0 && hole.area.height > 0) {
			int32_t inset_x = hole.area.x > 0
								  ? MANGO_MAX(1, lround(hole.area.x * scale_x))
								  : 0;
			int32_t inset_y = hole.area.y > 0
								  ? MANGO_MAX(1, lround(hole.area.y * scale_y))
								  : 0;
			int32_t hole_w =
				(int32_t)(meta->orig_width * scale_x) - 2 * inset_x;
			int32_t hole_h =
				(int32_t)(meta->orig_height * scale_y) - 2 * inset_y;
			hole.area.x = inset_x;
			hole.area.y = inset_y;
			hole.area.width = hole_w > 0 ? hole_w : 0;
			hole.area.height = hole_h > 0 ? hole_h : 0;
		}
		hole.corners.top_left = (uint16_t)(hole.corners.top_left * scale);
		hole.corners.top_right = (uint16_t)(hole.corners.top_right * scale);
		hole.corners.bottom_right =
			(uint16_t)(hole.corners.bottom_right * scale);
		hole.corners.bottom_left = (uint16_t)(hole.corners.bottom_left * scale);
		wlr_scene_rect_set_clipped_region(rect, hole);
		return;
	}

	if (node->type != WLR_SCENE_NODE_TREE) {
		return;
	}

	/* A snapshot buffer's wrapper is a tree node that carries the buffer's
	 * snapshot-absolute origin, so it moves exactly like a rect. */
	MangoSceneNode *payload = node->data;
	if (payload && payload->type == Snapshot && payload->owner &&
		clip->frame.width > 0 && clip->frame.height > 0) {
		SnapshotMetadata *meta = payload->owner;
		double scale_x = (double)clip->width / (double)clip->frame.width;
		double scale_y = (double)clip->height / (double)clip->frame.height;
		wlr_scene_node_set_position(
			node, frame_origin_scale(clip->frame.x, meta->orig_x, scale_x),
			frame_origin_scale(clip->frame.y, meta->orig_y, scale_y));
	}

	struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
	struct wlr_scene_node *child;
	wl_list_for_each(child, &tree->children, link) {
		scene_snapshot_scale_rects(child, clip);
	}
}

void scene_snapshot_clip_to_box(struct wlr_scene_tree *snapshot,
								const struct wlr_box *bounds, bool scale,
								int32_t width, int32_t height,
								const struct wlr_box *orig) {
	if (!snapshot || !bounds || bounds->width <= 0 || bounds->height <= 0) {
		return;
	}

	struct SnapshotClipData clip = {
		.bounds = *bounds,
		.width = width,
		.height = height,
		.scale = scale,
		.frame =
			orig ? *orig : (struct wlr_box){.width = width, .height = height},
	};
	scene_snapshot_enable_buffers(&snapshot->node);
	wlr_scene_node_for_each_buffer(&snapshot->node, scene_snapshot_clip_buffer,
								   &clip);
	if (scale) {
		scene_snapshot_scale_rects(&snapshot->node, &clip);
	}
}

static void scene_rect_fade(struct wlr_scene_node *node, double opacity) {
	if (node->type == WLR_SCENE_NODE_RECT) {
		MangoSceneNode *payload = node->data;
		if (!payload || payload->type != Snapshot || !payload->owner) {
			return;
		}

		SnapshotMetadata *meta = payload->owner;
		float faded[4] = {
			(float)(meta->orig_color[0] * opacity),
			(float)(meta->orig_color[1] * opacity),
			(float)(meta->orig_color[2] * opacity),
			(float)(meta->orig_color[3] * opacity),
		};
		wlr_scene_rect_set_color(wlr_scene_rect_from_node(node), faded);
		return;
	}

	if (node->type != WLR_SCENE_NODE_TREE) {
		return;
	}

	struct wlr_scene_tree *tree = wlr_scene_tree_from_node(node);
	struct wlr_scene_node *child;
	wl_list_for_each(child, &tree->children, link) {
		scene_rect_fade(child, opacity);
	}
}

/*
 * Fades a snapshot by `opacity`: buffers through their own opacity, rects
 * through their colour. The plain wlr_scene_node_for_each_buffer() walk used
 * before this only ever saw buffers, so the border ring and the title bar's
 * background stayed fully opaque for a whole close animation while everything
 * around them faded out.
 */
void scene_snapshot_fade(struct wlr_scene_node *node, double opacity) {
	if (!node) {
		return;
	}
	wlr_scene_node_for_each_buffer(node, scene_buffer_apply_opacity, &opacity);
	scene_rect_fade(node, opacity);
}
void request_fresh_all_monitors(void) {
	Monitor *m = NULL;
	wl_list_for_each(m, &server.monitors, link) {
		if (!m->wlr_output->enabled) {
			continue;
		}
		wlr_output_schedule_frame(m->wlr_output);
	}
}
