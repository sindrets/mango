#ifndef __ANIMATION_COMMON_H__
#define __ANIMATION_COMMON_H__ 1

#include <scenefx/types/fx/clipped_region.h>
#include <scenefx/types/wlr_scene.h>
#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>

#define BAKED_POINTS_COUNT 256

/* What is currently being animated / which animation phase is running. */
enum { NONE, OPEN, MOVE, CLOSE, TAG, FOCUS, OPAFADEIN, OPAFADEOUT, OVERVIEW };

struct dvec2 {
	double x, y;
};

struct ivec2 {
	int32_t x, y, width, height;
};

struct mango_animation {
	bool should_animate;
	bool running;
	bool tagining;
	bool tagouted;
	bool tagouting;
	bool begin_fade_in;
	bool tag_from_rule;
	bool overining;
	bool overview_enter_anim_set;
	uint32_t time_started;
	uint32_t duration;
	struct wlr_box initial;
	struct wlr_box current;
	int32_t action;
};

struct mango_opacity_animation {
	bool running;
	float current_opacity;
	float target_opacity;
	float initial_opacity;
	uint32_t time_started;
	uint32_t duration;
	float current_border_color[4];
	float target_border_color[4];
	float initial_border_color[4];
	float current_dim_color[4];
	float target_dim_color[4];
	float initial_dim_color[4];
};

typedef struct {
	float width_scale;
	float height_scale;
	int32_t width;
	int32_t height;
	struct fx_corner_radii corner_location;
	/* The blur covers the whole frame content, title bar included, so it keeps
	 * the rounded top corners the surface itself gives up when a bar is drawn.
	 */
	struct fx_corner_radii blur_corner_location;
	/* The clip that keeps the surface inside the frame's hole, in frame
	 * coordinates. A surface pushed down by a bar can't follow the frame's curve
	 * with its own radius, so it is clipped to the hole's shape; scene_buffer_apply_effect()
	 * converts it to buffer-local form. Empty when there is nothing to clip. */
	struct clipped_region clip;
	bool should_scale;
} BufferData;

typedef struct SnapshotMetadata {
	uint32_t type; // must at first in struct
	int32_t orig_width;
	int32_t orig_height;
	struct wlr_fbox orig_src_box;
	int32_t orig_x;
	int32_t orig_y;
	/* Premultiplied colour a snapshot rect was copied from. Rect nodes have no
	 * opacity of their own, so a fade scales their colour, and it has to scale
	 * the original rather than the faded result it wrote last tick. */
	float orig_color[4];
	/* Geometry a snapshot rect was copied from. A rect cannot be resized by a
	 * transform, so a zoom has to rewrite its box, its corners and its hole
	 * from these every tick. */
	struct fx_corner_radii orig_corners;
	struct clipped_region orig_clipped_region;
	bool is_subsurface;
	struct wl_listener destroy;
} SnapshotMetadata;

struct dvec2 calculate_animation_curve_at(double t, int32_t type);
void handle_snapshot_meta_destroy(struct wl_listener *listener, void *data);
void init_baked_points(void);
double find_animation_curve_at(double t, int32_t type);

bool scene_node_snapshot(struct wlr_scene_node *node, int32_t lx, int32_t ly,
						 struct wlr_scene_tree *snapshot_tree);

struct wlr_scene_tree *wlr_scene_tree_snapshot(struct wlr_scene_node *node,
											   struct wlr_scene_tree *parent);
/* `orig` is the frame the snapshot was taken at; a zoom needs it to map the
 * snapshot's rect nodes from that frame into the animated one. */
void scene_snapshot_clip_to_box(struct wlr_scene_tree *snapshot,
								const struct wlr_box *bounds, bool scale,
								int32_t width, int32_t height,
								const struct wlr_box *orig);
/* Fades a snapshot tree by `opacity`, buffers through their own opacity and
 * rects through their colour. */
void scene_snapshot_fade(struct wlr_scene_node *node, double opacity);
void request_fresh_all_monitors(void);
#endif
