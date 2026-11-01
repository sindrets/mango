#include "mango/ext-protocol/hdr.h"
#include "mango/manage/client.h"
#include <wlr/types/wlr_color_management_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_scene.h>

static uint32_t output_formats_8bit[10];
static uint32_t output_formats_10bit[8];
#include "mango/common/log.h"
#include "mango/common/server.h"
#include "mango/manage/monitor.h"
bool output_set_render_format(Monitor *m, uint32_t candidates[], size_t count,
							  struct wlr_output_state *state) {
	for (size_t i = 0; i < count; i++) {
		struct wlr_output_state test_state = *state;
		wlr_output_state_set_render_format(&test_state, candidates[i]);
		if (wlr_output_test_state(m->wlr_output, &test_state)) {
			wlr_output_state_set_render_format(state, candidates[i]);
			return true;
		}
	}

	mango_error(true, WLR_DEBUG, "HDR: Failed to set render format");
	return false;
}

bool output_format_in_candidates(uint32_t format, uint32_t candidates[],
								 size_t count) {
	for (size_t i = 0; i < count; i++)
		if (candidates[i] == format)
			return true;
	return false;
}
enum render_bit_depth bit_depth_from_format(uint32_t render_format) {
	if (output_format_in_candidates(render_format, output_formats_10bit,
									ARRAY_SIZE(output_formats_10bit)))
		return MANGO_RENDER_BIT_DEPTH_10;
	if (output_format_in_candidates(render_format, output_formats_8bit,
									ARRAY_SIZE(output_formats_8bit)))
		return MANGO_RENDER_BIT_DEPTH_8;
	return MANGO_RENDER_BIT_DEPTH_DEFAULT;
}
bool output_supports_hdr(const Monitor *m, const char **reason) {
	const struct wlr_output *output = m->wlr_output;
	const char *r = NULL;

	// hdr_force skips the two EDID-derived checks, for panels that declare HDR
	// only inside a DisplayID 2.0 extension. It does not skip the third:
	// output_color_transform is a renderer capability, not an EDID claim.
	if (!m->hdr_force &&
		!(output->supported_primaries & WLR_COLOR_NAMED_PRIMARIES_BT2020))
		r = "BT2020 primaries not supported";
	else if (!m->hdr_force && !(output->supported_transfer_functions &
								WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ))
		r = "PQ transfer function not supported";
	else if (!server.renderer->features.output_color_transform)
		r = "renderer doesn't support output color transforms";
	if (reason)
		*reason = r;
	return !r;
}
void output_enable_hdr(Monitor *m, struct wlr_output_state *os, bool enabled,
					   bool silent) {
	/* Never short-circuit the disable path. wlroots only recomputes the scene
	 * colour transform for commits that carry
	 * WLR_OUTPUT_STATE_IMAGE_DESCRIPTION, so a scene still holding the PQ
	 * transform would keep encoding every frame for HDR on a connector that
	 * already went back to SDR. Clearing an already-clear description is
	 * harmless, so disable always does the work. */

	if (!output_supports_hdr(m, NULL)) {
		m->is_hdr_enabling = false;
		return;
	}

	if (!enabled) {
		if (m->wlr_output->supported_primaries ||
			m->wlr_output->supported_transfer_functions) {
			if (!silent)
				mango_error(true, WLR_DEBUG, "Disabling HDR on output %s",
							m->wlr_output->name);
			wlr_output_state_set_image_description(os, NULL);
		}
		m->is_hdr_enabling = false;
		return;
	}

	if (!silent)
		mango_error(true, WLR_DEBUG, "Enabling HDR on output %s",
					m->wlr_output->name);
	struct wlr_output_image_description desc = {
		.primaries = WLR_COLOR_NAMED_PRIMARIES_BT2020,
		.transfer_function = WLR_COLOR_TRANSFER_FUNCTION_ST2084_PQ,
	};

	// Mastering display metadata, from the monitorrule: wlroots does not expose
	// the EDID luminances. Values are in cd/m²; 0 means unset and leaves the
	// field zero, which is the previous behaviour.
	if (m->hdr_max_lum > 0) {
		wlr_color_primaries_from_named(&desc.mastering_display_primaries,
									   WLR_COLOR_NAMED_PRIMARIES_BT2020);
		desc.mastering_luminance.min = m->hdr_min_lum;
		desc.mastering_luminance.max = m->hdr_max_lum;
		desc.max_cll = m->hdr_max_lum;
		desc.max_fall = m->hdr_max_avg_lum;
	}

	m->is_hdr_enabling = true;
	wlr_output_state_set_image_description(os, &desc);
}
void output_state_setup_hdr(Monitor *m, bool silent,
							struct wlr_output_state *state) {
	uint32_t render_format = m->wlr_output->render_format;
	const char *unsupported_reason = NULL;
	bool hdr_supported = output_supports_hdr(m, &unsupported_reason);
	bool hdr_succeeded = false;

	if (!hdr_supported) {
		if (!silent)
			mango_error(true, WLR_INFO, "HDR not supported on output %s: %s",
						m->wlr_output->name, unsupported_reason);
		return;
	}

	enum render_bit_depth depth = config.hdr_depth;
	if (depth == MANGO_RENDER_BIT_DEPTH_DEFAULT)
		depth = bit_depth_from_format(render_format);

	if (depth == MANGO_RENDER_BIT_DEPTH_10 &&
		bit_depth_from_format(render_format) == depth) {
		hdr_succeeded = true; // 10-bit was already set successfully last time;
							  // reuse it directly.
	} else if (depth == MANGO_RENDER_BIT_DEPTH_10) {
		hdr_succeeded = output_set_render_format(
			m, output_formats_10bit, ARRAY_SIZE(output_formats_10bit), state);
		if (!hdr_succeeded) {
			if (!silent)
				mango_error(true, WLR_INFO,
							"No 10 bit color formats supported, HDR disabled.");
			hdr_succeeded = output_set_render_format(
				m, output_formats_8bit, ARRAY_SIZE(output_formats_8bit), state);
			if (!hdr_succeeded) {
				if (!silent)
					mango_error(true, WLR_ERROR,
								"No 8 bit color formats supported!");
			}
		}
	} else {
		// Explicitly requires 8-bit or falls back automatically.
		hdr_succeeded = output_set_render_format(
			m, output_formats_8bit, ARRAY_SIZE(output_formats_8bit), state);
		if (!hdr_succeeded) {
			if (!silent)
				mango_error(true, WLR_ERROR,
							"No 8 bit color formats supported!");
		}
	}

	output_enable_hdr(m, state, hdr_succeeded, silent);
}
/* togglehdr[,on|off|toggle][,<monitor name>|all] -- apply to one output */
bool togglehdr_output(Monitor *target, bool want) {
	if (!target || !target->wlr_output->enabled || !target->scene_output)
		return false;

	/* Only "already in HDR" is a true no-op. Leaving HDR is always performed,
	 * even when the bookkeeping flags already say SDR: the output can have
	 * gone back to SDR on the connector while the scene still holds the PQ
	 * colour transform, in which case every later frame keeps being encoded
	 * for HDR and the screen stays washed out until something forces a real
	 * commit that carries WLR_OUTPUT_STATE_IMAGE_DESCRIPTION. Clearing an
	 * already-clear description is idempotent, so this costs one commit on a
	 * redundant `togglehdr off` and guarantees the recovery path works. */
	if (want && want == target->hdr_enable)
		return false;

	// Check before mutating, so a refusal leaves the state untouched. This is
	// also what makes the "all" form safe when some outputs are SDR.
	const char *reason = NULL;
	if (want && !output_supports_hdr(target, &reason)) {
		wlr_log(WLR_INFO, "togglehdr: HDR unavailable on %s: %s",
				target->wlr_output->name, reason);
		return false;
	}

	// Snapshot both flags: output_enable_hdr() writes is_hdr_enabling before
	// the commit is attempted, so a failed commit must restore both or the two
	// disagree -- mango would render SDR on a connector still in PQ.
	bool prev_enable = target->hdr_enable;
	bool prev_enabling = target->is_hdr_enabling;

	target->hdr_enable = want;

	if (want)
		output_state_setup_hdr(target, false, &target->pending);
	else
		output_enable_hdr(target, &target->pending, false, false);

	// wlroots damages the whole output for a geometry, transform, scale or
	// rendered gamma LUT change, but not for an image description change, which
	// also changes how every pixel is encoded. Without this, anything left
	// undamaged keeps the luminance mapping it had when last drawn.
	wlr_output_schedule_frame(target->wlr_output);
	wlr_damage_ring_add_whole(&target->scene_output->damage_ring);

	// Swapping the image description reconfigures the output; wlroots refuses
	// such a commit unless the disruption is explicitly allowed.
	target->pending.allow_reconfiguration = true;

	if (!mango_scene_output_commit(target->scene_output, &target->pending)) {
		wlr_log(WLR_ERROR, "togglehdr: commit failed on %s, reverting",
				target->wlr_output->name);
		target->hdr_enable = prev_enable;
		target->is_hdr_enabling = prev_enabling;
		// The commit helper only recycles pending on success.
		wlr_output_state_finish(&target->pending);
		wlr_output_state_init(&target->pending);
		return false;
	}

	wlr_output_effective_resolution(target->wlr_output, &target->m.width,
									&target->m.height);
	return true;
}
int32_t toggle_hdr(const Arg *arg) {
	// arg->i: 1 = on, 0 = off, -1 = toggle (also the default when no argument
	// was given, so a bare `togglehdr` binding does the obvious thing).
	if (arg->v && strcmp(arg->v, "all") == 0) {
		Monitor *m = NULL;
		bool want;

		if (arg->i < 0) {
			// One decision for every output: flipping each against its own
			// state would leave a multi-monitor desk half on and half off, and
			// the next press would swap the halves instead of converging.
			bool any_on = false;
			wl_list_for_each(m, &server.monitors, link) {
				if (m->wlr_output->enabled && m->hdr_enable) {
					any_on = true;
					break;
				}
			}
			want = !any_on;
		} else {
			want = arg->i != 0;
		}

		wl_list_for_each(m, &server.monitors, link) {
			/* A manual toggle takes the output back from auto-HDR, otherwise
			 * the next fullscreen HDR client would silently undo it. */
			m->auto_hdr_active = false;
			m->auto_hdr_client = NULL;
			togglehdr_output(m, want);
		}
		return 0;
	}

	Monitor *m = NULL, *target = NULL;

	if (arg->v && *arg->v) {
		wl_list_for_each(m, &server.monitors, link) {
			if (m->wlr_output->enabled &&
				strcmp(m->wlr_output->name, arg->v) == 0) {
				target = m;
				break;
			}
		}
		if (!target) {
			wlr_log(WLR_ERROR, "togglehdr: no enabled output named %s", arg->v);
			return 0;
		}
	} else {
		target = server.selected_monitor;
	}

	if (!target)
		return 0;

	/* A manual toggle takes the output back from auto-HDR. */
	target->auto_hdr_active = false;
	target->auto_hdr_client = NULL;

	togglehdr_output(target, arg->i < 0 ? !target->hdr_enable : (arg->i != 0));
	return 0;
}

static uint32_t output_formats_8bit[] = {
	DRM_FORMAT_XRGB8888, DRM_FORMAT_XBGR8888, DRM_FORMAT_RGBX8888,
	DRM_FORMAT_BGRX8888, DRM_FORMAT_ARGB8888, DRM_FORMAT_ABGR8888,
	DRM_FORMAT_RGBA8888, DRM_FORMAT_BGRA8888, DRM_FORMAT_RGB888,
	DRM_FORMAT_BGR888,
};

static uint32_t output_formats_10bit[] = {
	DRM_FORMAT_XRGB2101010, DRM_FORMAT_XBGR2101010, DRM_FORMAT_RGBX1010102,
	DRM_FORMAT_BGRX1010102, DRM_FORMAT_ARGB2101010, DRM_FORMAT_ABGR2101010,
	DRM_FORMAT_RGBA1010102, DRM_FORMAT_BGRA1010102,
};

/*
 * Auto-HDR
 * ========
 *
 * Hyprland's `render:cm_auto_hdr` keys off the client itself: the compositor
 * looks at the wp_color_manager_v1 image description a fullscreen client
 * committed and, when that description is HDR (PQ, HLG or scRGB), bumps the
 * output into HDR. Nothing in the protocol says "this window wants HDR" -- the
 * declaration of a HDR image description *is* the request. See
 * CWLSurfaceResource::findWithCM() / CColorManagementSurface::isHDR() and
 * IHyprRenderer::handleFullscreenSettings() upstream.
 *
 * The second half is what makes a CM-aware client like gamescope actually
 * submit HDR content: the compositor advertises, through
 * wp_color_management_surface_feedback_v1, the image description it would like
 * the surface to use. Hyprland returns the would-be HDR description from
 * CMonitor::getAutoHDRImageDescription() while the client is fullscreen.
 * wlroots already re-sends a preferred description derived from the outputs in
 * play (types/scene/surface.c), but that can never be HDR while the output is
 * SDR, so a chicken-and-egg problem exists unless the compositor overrides it.
 * mango therefore overrides it explicitly for the fullscreen client.
 */

static bool
image_desc_is_hdr(const struct wlr_image_description_v1_data *desc) {
	switch (desc->tf_named) {
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ:
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_HLG:
		return true;
	case WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR:
		/* scRGB: extended-range linear light on sRGB primaries. */
		return desc->primaries_named == WP_COLOR_MANAGER_V1_PRIMARIES_SRGB;
	default:
		return false;
	}
}

/* Same pre-order walk Hyprland's findWithCM() does: root first, then the
 * subsurface tree, looking for the first surface that committed an image
 * description. A gamescope surface typically sets it on the root, but some
 * clients set it on a child, so the walk matters. */
struct hdr_surface_lookup {
	const struct wlr_image_description_v1_data *desc;
};

static void hdr_surface_lookup_iterator(struct wlr_surface *surface, int sx,
										int sy, void *data) {
	struct hdr_surface_lookup *lookup = data;
	if (lookup->desc != NULL)
		return;
	const struct wlr_image_description_v1_data *desc =
		wlr_surface_get_image_description_v1_data(surface);
	if (desc != NULL)
		lookup->desc = desc;
}

/* Returns the image description the toplevel (or any of its subsurfaces)
 * committed, or NULL when the surface does not take part in
 * wp_color_manager_v1. */
static const struct wlr_image_description_v1_data *
client_image_description(Client *c) {
	if (!c || client_is_x11(c))
		return NULL; /* XWayland windows have no wp_color_manager_v1 */

	struct wlr_surface *root = client_surface(c);
	if (!root)
		return NULL;

	/* wlr_surface_for_each_surface() walks in rendering order and includes
	 * the root, which is exactly what findWithCM() covers. */
	struct hdr_surface_lookup lookup = {0};
	wlr_surface_for_each_surface(root, hdr_surface_lookup_iterator, &lookup);
	return lookup.desc;
}

bool client_advertises_hdr(Client *c) {
	const struct wlr_image_description_v1_data *desc =
		client_image_description(c);
	return desc != NULL && image_desc_is_hdr(desc);
}

/* The client that auto-HDR should consider for a monitor: the visible
 * fullscreen client, but only while it is actually on screen and focused
 * enough to be the thing the user is looking at. */
static Client *auto_hdr_target_client(Monitor *m) {
	if (!m || !m->wlr_output->enabled)
		return NULL;

	Client *c = client_focus_top(m);
	if (!c || c->iskilling || !VISIBLEON(c, m) || !c->isfullscreen)
		return NULL;
	/* A client with a window rule opting out keeps the output as-is. */
	if (!c->auto_hdr)
		return NULL;

	return c;
}

/* Builds the image description to advertise to a surface. */
static void
hdr_preferred_description(Monitor *m,
						  struct wlr_image_description_v1_data *out) {
	*out = (struct wlr_image_description_v1_data){
		.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ,
		.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020,
	};

	if (m->hdr_max_lum > 0) {
		out->has_mastering_display_primaries = true;
		wlr_color_primaries_from_named(&out->mastering_display_primaries,
									   WLR_COLOR_NAMED_PRIMARIES_BT2020);
		out->has_mastering_luminance = true;
		out->mastering_luminance.min = m->hdr_min_lum;
		out->mastering_luminance.max = m->hdr_max_lum;
		out->max_cll = (uint32_t)m->hdr_max_lum;
		out->max_fall = (uint32_t)m->hdr_max_avg_lum;
	}
}

struct hdr_preferred_apply {
	struct wlr_color_manager_v1 *manager;
	const struct wlr_image_description_v1_data *desc;
};

static void hdr_preferred_apply_iterator(struct wlr_surface *surface, int sx,
										 int sy, void *data) {
	struct hdr_preferred_apply *apply = data;
	wlr_color_manager_v1_set_surface_preferred_image_description(
		apply->manager, surface, apply->desc);
}

/* Tells every surface of `c` the image description we would like it to use.
 * While auto-HDR is armed this is the HDR description (so gamescope and friends
 * switch to PQ), otherwise NULL asks wlroots to fall back to the description
 * derived from the outputs (see wlr_scene's
 * get_surface_preferred_image_description()). */
static void client_set_preferred_description(Client *c, Monitor *m, bool hdr) {
	if (!server.color_manager || !c || client_is_x11(c))
		return;

	struct wlr_surface *root = client_surface(c);
	if (!root)
		return;

	struct wlr_image_description_v1_data desc;
	if (hdr) {
		hdr_preferred_description(m, &desc);
	} else {
		/* Fall back to what the outputs in play imply. */
		desc = (struct wlr_image_description_v1_data){
			.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_GAMMA22,
			.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_SRGB,
		};
		if (m && m->is_hdr_enabling) {
			desc.tf_named = WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ;
			desc.primaries_named = WP_COLOR_MANAGER_V1_PRIMARIES_BT2020;
		}
	}

	/* Advertise on the whole surface tree: clients bind surface feedback
	 * per wl_surface, so a description set on the root alone would not reach
	 * a subsurface that holds the buffer. */
	struct hdr_preferred_apply apply = {
		.manager = server.color_manager,
		.desc = &desc,
	};
	wlr_surface_for_each_surface(root, hdr_preferred_apply_iterator, &apply);
}

/* Applies or reverts auto-HDR on one monitor. Returns whether anything
 * changed. */
bool auto_hdr_apply(Monitor *m, bool want) {
	if (!m || !m->wlr_output->enabled || !m->scene_output)
		return false;

	if (!want) {
		/* Give the output back to whatever it was doing before.
		 * auto_hdr_restore is only meaningful while we own the state. */
		if (!m->auto_hdr_active)
			return false;
		bool restore = m->auto_hdr_restore;
		m->auto_hdr_active = false;
		togglehdr_output(m, restore);
		return true;
	}

	/* Already in the right state, and auto-HDR still owns it. */
	if (m->auto_hdr_active && m->hdr_enable)
		return false;

	if (!m->auto_hdr_active) {
		/* First time auto-HDR touches this output: remember what to go
		 * back to when the fullscreen client leaves. */
		m->auto_hdr_restore = m->hdr_enable;
	}

	if (!togglehdr_output(m, true)) {
		/* Refused (unsupported output) or a no-op; drop ownership so a
		 * later request retries from a clean slate. */
		m->auto_hdr_active = false;
		return false;
	}

	m->auto_hdr_active = true;
	return true;
}

/*
 * Re-evaluates auto-HDR for one monitor.
 *
 * Two rules drive the ordering here:
 *
 * - The preferred description is advertised before the output is switched, so
 *   a client that is willing to go HDR does so on its next commit. The switch
 *   itself is driven by what the client *committed*, not by what we
 *   advertised: a client that ignores the advertisement stays SDR instead of
 *   getting a PQ output showing SDR content.
 * - The advertisement is rolled back after the output has left HDR, so the
 *   rollback is not itself a PQ advertisement.
 */
static void check_auto_hdr_monitor(Monitor *m) {
	if (!m || !m->wlr_output->enabled)
		return;

	/* Only advertise HDR to a client whose output we can actually switch;
	 * advertising PQ on an output that stays SDR would make the client render
	 * HDR into an SDR pipe. */
	bool enabled = config.cm_auto_hdr && output_supports_hdr(m, NULL);
	Client *target = enabled ? auto_hdr_target_client(m) : NULL;
	Client *prev = m->auto_hdr_client;

	/* A different client gets a fresh attempt at switching the output. */
	if (prev != target)
		m->auto_hdr_failed = false;
	m->auto_hdr_client = target;

	bool want = false;
	if (target) {
		client_set_preferred_description(target, m, true);
		/* The client reacts to the advertisement on its own schedule: it has
		 * to round-trip through its event loop and commit again. The HDR
		 * switch therefore also happens from check_auto_hdr_committed();
		 * what the client committed is the truth. */
		want = client_advertises_hdr(target);
		m->auto_hdr_client_hdr = want;
	}

	if (want) {
		auto_hdr_apply(m, true);
		if (!m->hdr_enable)
			/* The output rejected the switch (unsupported, or the commit
			 * failed). Retrying on every frame commit would just spam the
			 * log, so wait for the next client/fullscreen change. */
			m->auto_hdr_failed = true;
	} else if (m->auto_hdr_active) {
		/* Leave HDR first: the rollback below must not be a PQ
		 * advertisement. */
		auto_hdr_apply(m, false);
	}

	/* Roll back the advertisement for a client that stopped being the target
	 * (unfocused, unfullscreened, ...): otherwise it keeps rendering PQ into
	 * an output that is no longer in HDR. */
	if (prev && prev != target && !prev->iskilling)
		client_set_preferred_description(prev, m, false);
}

/*
 * Auto-HDR is evaluated from an idle callback rather than inline.
 *
 * Checkpoints (client commit, focus, fullscreen change, unmap) can fire while
 * wlroots is in the middle of committing a surface, and switching HDR runs a
 * full output commit. Doing that from inside a surface commit handler would
 * re-enter the scene commit machinery; deferring also coalesces the burst of
 * commits a client produces when it reacts to our advertisement.
 */
static void auto_hdr_idle(void *data) {
	Monitor *m = data;

	m->auto_hdr_idle = NULL;

	/* Outputs can go away while the evaluation is queued. */
	Monitor *iter = NULL;
	bool found = false;
	wl_list_for_each(iter, &server.monitors, link) {
		if (iter == m) {
			found = true;
			break;
		}
	}
	if (!found)
		return;

	check_auto_hdr_monitor(m);
}

/* Queues an evaluation for one output; a no-op when it is already queued. */
static void schedule_auto_hdr(Monitor *m) {
	if (!m || !m->wlr_output->enabled || m->auto_hdr_idle)
		return;
	m->auto_hdr_idle =
		wl_event_loop_add_idle(server.event_loop, auto_hdr_idle, m);
}

/* Called from the focus / fullscreen paths, mirroring check_vrr_enable(). */
void check_auto_hdr(Client *c) {
	Monitor *m = c && c->mon ? c->mon : server.selected_monitor;

	schedule_auto_hdr(m);

	/* Another monitor may have lost the client that was keeping it in HDR
	 * (focus moved, or the window was destroyed). */
	Monitor *iter = NULL;
	wl_list_for_each(iter, &server.monitors, link) {
		if (iter == m)
			continue;
		if (iter->auto_hdr_active || iter->auto_hdr_client)
			schedule_auto_hdr(iter);
	}
}

/* Called when the raw surface color state changes, i.e. when a fullscreen
 * client commits HDR content in response to our advertisement. */
void check_auto_hdr_committed(Client *c) {
	if (!c || !c->mon)
		return;

	Monitor *m = c->mon;

	/* Nobody cares unless this client is the auto-HDR target (which requires
	 * it to be fullscreen), or it is fullscreen and just went HDR -- the
	 * latter is what promotes it to target. */
	if (m->auto_hdr_client != c && !(c->isfullscreen && VISIBLEON(c, m)))
		return;

	/* Cheap early-out: clients commit on every frame, while a full evaluation
	 * walks the surface tree and may re-advertise. Only re-evaluate when the
	 * HDR-ness of the committed description changed, or when the output state
	 * does not match it yet. */
	bool hdr = client_advertises_hdr(c);
	if (m->auto_hdr_client == c) {
		/* Settled when the output state already matches the committed
		 * description, or when switching it was given up on. */
		if (hdr == m->auto_hdr_client_hdr &&
			(m->hdr_enable == hdr || m->auto_hdr_failed))
			return;
		m->auto_hdr_client_hdr = hdr;
	} else if (!hdr)
		return;

	schedule_auto_hdr(m);
}

/* Re-applies auto-HDR to every output, e.g. after a config reload. */
void check_auto_hdr_all(void) {
	Monitor *m = NULL;
	wl_list_for_each(m, &server.monitors, link) schedule_auto_hdr(m);
}
