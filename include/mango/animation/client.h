#ifndef __ANIMATION_CLIENT_H__
#define __ANIMATION_CLIENT_H__ 1

#include "mango/animation/common.h"
#include "mango/common/types.h"
#include <stdbool.h>

bool client_is_ignore_output_clip(Client *c);
struct ivec2 compute_edge_offsets(Client *c);

/* Height the title bar occupies in the frame right now (0 when hidden), and
 * the same clamped to a frame height being drawn (animations shrink frames). */
void client_title_bar_draw_height(Client *c, int32_t frame_height,
								  int32_t *bar_height);
void client_actual_size(Client *client, int32_t *width, int32_t *height);
void set_rect_size(struct wlr_scene_rect *rect, int32_t width, int32_t height);

struct fx_corner_radii set_client_corner_location(Client *client);

// Radius the inner edge of the client's border has. Surfaces and the hole cut
// for them sit inset by the border width, so they need this instead of the
// outer radius set_client_corner_location() reports.
struct fx_corner_radii
client_inner_corner_location(Client *client, struct fx_corner_radii outer);

// Corners for the client surface itself. Same as the inner corners, except
// that the top corners go square while a title bar is drawn: the surface is
// pushed below the bar, so its own top arc would land under the bar and leave
// a wedge of background at the frame's corner. The bar draws that arc instead.
struct fx_corner_radii
client_surface_corner_location(Client *client, struct fx_corner_radii outer);

// The hole the border cuts for the frame's contents, in frame coordinates. The
// title bar and the client surface both live inside it, which is what makes the
// frame read as one piece with the border. The surface is clipped to it when a
// title bar pushes it down, so the two curves stay on top of each other.
struct clipped_region client_frame_hole(Client *client, struct ivec2 offsets);

// Where the frame's hole sits relative to the client surface's own tree, i.e.
// what the surface has to stay inside of. Empty when no title bar pushes the
// surface down, because then its own corner radius already follows the frame.
struct clipped_region client_surface_clip(Client *client, struct ivec2 offsets);

bool is_horizontal_stack_layout(Monitor *monitor);
bool is_horizontal_right_stack_layout(Monitor *monitor);

int32_t is_special_animation_rule(Client *client);
void set_overview_enter_animation(Client *c);
void set_client_open_animation(Client *c, struct wlr_box geo);

void scene_buffer_apply_effect(struct wlr_scene_buffer *buffer, int32_t sx,
							   int32_t sy, void *data);
void buffer_set_effect(Client *c, BufferData data);

void client_draw_shadow(Client *c, struct ivec2 offsets);
void client_draw_title_bar(Client *c, struct ivec2 offsets);
void client_draw_group_bar(Client *c, struct ivec2 offsets);
void global_draw_group_bar(Client *c, int32_t x, int32_t y, int32_t width,
						   int32_t height);
void client_draw_shield(Client *c, struct ivec2 clip_box);
void client_draw_dim(Client *c, struct ivec2 clip_box);
void client_draw_blur(Client *c, struct ivec2 clip_box);
void client_draw_split_border(Client *c, bool hit_no_border,
							  struct ivec2 offsets);
void client_draw_border(Client *c, struct ivec2 offsets);
struct ivec2 clip_to_hide(Client *c, struct wlr_box *clip_box,
						  struct ivec2 offsets);
void client_set_drop_area(Client *c);

/* ---------- central rendering entry point ---------- */
void client_set_clip(Client *c, float factor);
void client_animation_set_progress(Client *c, double p);
void client_animation_resume(Client *c, double remaining);
void fadeout_client_animation_next_tick(Client *c);
void client_animation_next_tick(Client *c);
void init_fadeout_client(Client *c);
bool client_animations_enabled(const Client *c);

/* Applies the final window state when animation is disabled: position,
 * clipping/visibility, and geometry state are synchronized. */
void client_set_finish_geometry(Client *c);
void client_commit(Client *c);
void client_set_pending_state(Client *c);

typedef struct ResizeOpts {
	bool interact; // Interactive resize (window resized by mouse drag).
	bool skip_ov_enter_anim; // Pre-arrangement stage: skip the overview enter
							 // zoom.
	bool force_configure; // Must be sent even if the box did not change (client
						  // ConfigureRequest).
	uint32_t drag_edge;
} ResizeOpts;

void resize(Client *c, struct wlr_box geo, ResizeOpts opts);
bool client_draw_fadeout_frame(Client *c);
void client_set_focused_opacity_animation(Client *c);
void client_set_unfocused_opacity_animation(Client *c);
void client_start_focus_animation(Client *c, bool focused);
bool client_step_focus_animation(Client *c, double linear_progress);
bool client_set_focus_opacity(Client *c);
bool client_draw_frame(Client *c);

#endif
