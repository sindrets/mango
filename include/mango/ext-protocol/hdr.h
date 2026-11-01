#ifndef __EXT_PROTOCOL_HDR_H__
#define __EXT_PROTOCOL_HDR_H__ 1

#include "mango/common/types.h"
#include "mango/config/parse.h"
#include "mango/dispatch/bind.h"
#include <drm_fourcc.h>
#include <stdbool.h>
#include <stdint.h>

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

/* Declarations */

bool output_set_render_format(Monitor *m, uint32_t candidates[], size_t count,
							  struct wlr_output_state *state);
bool output_format_in_candidates(uint32_t format, uint32_t candidates[],
								 size_t count);
enum render_bit_depth bit_depth_from_format(uint32_t render_format);
bool output_supports_hdr(const Monitor *m, const char **reason);
void output_enable_hdr(Monitor *m, struct wlr_output_state *os, bool enabled,
					   bool silent);
void output_state_setup_hdr(Monitor *m, bool silent,
							struct wlr_output_state *state);
/* togglehdr[,on|off|toggle][,<monitor name>|all] -- apply to one output */
bool togglehdr_output(Monitor *target, bool want);
int32_t toggle_hdr(const Arg *arg);

/*
 * Auto-HDR (cm_auto_hdr): switch a monitor to HDR while a fullscreen client
 * commits an HDR image description over wp_color_manager_v1.
 */
bool client_advertises_hdr(Client *c);
bool auto_hdr_apply(Monitor *m, bool want);
void check_auto_hdr(Client *c);
void check_auto_hdr_committed(Client *c);
void check_auto_hdr_all(void);
/* wlr_color_manager_v1.events.client_wants_hdr handler; see hdr.c. */
void hdr_handle_client_wants_hdr(struct wl_listener *listener, void *data);
#endif
