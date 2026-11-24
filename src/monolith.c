/*
 * MONOLITH - a slot-strip Wayland compositor for HDXI.
 *
 * Model: a horizontal strip of slots. One app per slot. The focused slot is
 * shown large in the middle, its direct neighbours peek in from the sides.
 * A slot can be flipped into "windows mode" where its windows float normally.
 * Popups / dialogs (toplevels with a parent) join the parent's slot and flip
 * it to windows mode automatically.
 *
 * Not implemented yet (see docs/ARCHITECTURE.md): fullscreen, multi-monitor.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <fnmatch.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>

#include <drm_fourcc.h>
#include <libinput.h>
#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/libinput.h>
#include <wlr/render/allocator.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/pass.h>
#include <wlr/render/swapchain.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_foreign_registry.h>
#include <wlr/types/wlr_xdg_foreign_v1.h>
#include <wlr/types/wlr_xdg_foreign_v2.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <wlr/version.h>
#include <xkbcommon/xkbcommon.h>

#include "config.h"

#ifndef BTN_LEFT
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#endif
#define MIN_WIN_W 160
#define MIN_WIN_H 100

#define MODS_MASK (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)

enum kind { K_VIEW, K_LAYER, K_SLOT }; /* first member of view/layer/img_tag, stored in node.data */

struct slot;
struct img_tag { enum kind kind; struct slot *slot; };

struct view {
	enum kind kind;
	struct wl_list link;            /* in slot->views */
	struct slot *slot;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;
	bool mapped;
	int cur_w, cur_h;               /* last size we asked the client for */
	int float_x, float_y, float_w, float_h; /* windows-mode geometry, 0 = unset */

	struct wl_listener map, unmap, commit, destroy;
	struct wl_listener request_move, request_resize, request_maximize, request_fullscreen;
};

struct slot {
	struct wl_list link;            /* in server.slots, left to right */
	struct wlr_scene_tree *tree;    /* strip-mode window (main app), in the strip layer */
	struct wlr_scene_tree *ftree;   /* windows-mode windows, in the floating layer above */
	struct wl_list views;
	struct view *focus_view;
	bool windows_mode;
	bool auto_wm;                   /* windows mode was set by a popup, may revert */

	/* tilted/dimmed picture of this slot, used while it is a neighbour or moving */
	struct wlr_scene_buffer *img;
	struct img_tag tag;
	struct wlr_swapchain *flat_sc, *out_sc;
	int flat_w, flat_h, out_w, out_h;
	int16_t *col_t, *col_b;         /* per output column: where the picture is solid (hit-testing) */
	bool img_active;
};

struct layer {
	enum kind kind;
	struct wl_list link;
	struct wlr_layer_surface_v1 *ls;
	struct wlr_scene_layer_surface_v1 *scene;
	bool mapped;
	struct wl_listener map, unmap, commit, destroy, new_popup;
};

struct output {
	struct wlr_output *wlr_output;
	int w, h;
	struct wl_listener frame, request_state, commit, destroy;
};

struct keyboard {
	struct wl_list link;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers, key, destroy;
};

struct deco {
	struct wlr_xdg_toplevel_decoration_v1 *wlr;
	struct wl_listener request_mode, destroy;
};

struct popup_l {
	struct wlr_xdg_popup *popup;
	struct wlr_scene_tree *root;    /* scene tree of the toplevel / layer surface it hangs off */
	struct wl_listener destroy, reposition, commit;
};

static struct server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_output_layout *output_layout;
	struct wlr_scene_rect *bg;
	struct wlr_scene_tree *t_hidden, *t_layer[4], *t_strip, *t_float;

	struct wlr_xdg_shell *xdg_shell;
	struct wlr_layer_shell_v1 *layer_shell;
	struct wlr_xdg_decoration_manager_v1 *deco_mgr;
	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wlr_cursor_shape_manager_v1 *cursor_shape;
	struct wlr_seat *seat;

	struct wl_list keyboards;
	struct wl_list slots;
	struct wl_list layers;
	struct slot *focused, *last_slot;
	struct view *active_view;
	struct layer *focused_layer;
	struct output *output;
	struct wlr_box usable;

	struct wlr_scene_tree *t_side;  /* tilted neighbour pictures */
	double pos;                     /* strip position: index of the slot in the middle (fractional while animating) */
	bool have_pos, animating, img_dirty, img_failed;
	double anim_from, anim_to;
	int64_t anim_t0;
	struct wl_event_source *img_timer;

	int ipc_fd;
	char ipc_path[256];

	struct wl_listener new_output, new_xdg_toplevel, new_xdg_popup, new_layer_surface,
		new_decoration, new_input, request_cursor, request_set_selection,
		request_set_primary, cursor_motion, cursor_motion_abs, cursor_button,
		cursor_axis, cursor_frame, request_shape,
		swipe_begin, swipe_update, swipe_end;
} S;

static struct config cfg;
static char *cfg_path;

/* interactive move/resize of floating windows (windows mode) */
enum grab_mode { GRAB_NONE, GRAB_MOVE, GRAB_RESIZE };
static struct {
	enum grab_mode mode;
	struct view *view;
	double cx, cy;              /* cursor position at grab start */
	int x, y, w, h;             /* window rectangle at grab start (slot-relative) */
	uint32_t edges;
} grab;

/* ------------------------------------------------------------------ */
/* spawning + sound                                                    */
/* ------------------------------------------------------------------ */

static pid_t spawn_argv(char *const *argv, bool quiet) {
	pid_t pid = fork();
	if (pid < 0) return -1;
	if (pid == 0) {
		sigset_t set;
		sigemptyset(&set);
		sigprocmask(SIG_SETMASK, &set, NULL);
		setsid();
		if (quiet) {
			int fd = open("/dev/null", O_RDWR);
			/* keep stderr with -v so a failing player explains itself in the log */
			if (fd >= 0) {
				dup2(fd, 0); dup2(fd, 1);
				if (wlr_log_get_verbosity() < WLR_INFO) dup2(fd, 2);
				if (fd > 2) close(fd);
			}
		}
		execvp(argv[0], argv);
		_exit(127);
	}
	return pid;
}

static void spawn_shell(const char *cmd) {
	char *argv[] = { "/bin/sh", "-c", (char *)cmd, NULL };
	spawn_argv(argv, false);
}

static struct { pid_t pid; struct timespec last; } snd;

static int sigchld_handler(int sig, void *data) {
	(void)sig; (void)data;
	pid_t p;
	int st;
	while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
		if (p != snd.pid) continue;
		snd.pid = 0;
		if (WIFEXITED(st) && WEXITSTATUS(st) != 0)
			wlr_log(WLR_ERROR, "sound command exited with status %d (127 = not found in PATH); run it by hand to see why", WEXITSTATUS(st));
	}
	return 0;
}

static void log_effective_config(void) {
	static const char *names[SND_COUNT] = { "slot_switch", "slot_open", "slot_close", "windows_mode" };
	wlr_log(WLR_INFO, "effective: popups=%s same_client=%s sound=%s", cfg.popups_auto ? "auto" : "off",
		cfg.same_client_join ? "join" : "new", cfg.sound_enabled ? "on" : "off");
	for (int i = 0; i < SND_COUNT; i++) {
		if (cfg.sound_cmd[i]) wlr_log(WLR_INFO, "effective: sound.%s = %s %s ...", names[i], cfg.sound_cmd[i][0],
			cfg.sound_cmd[i][1] ? cfg.sound_cmd[i][1] : "");
		else wlr_log(WLR_INFO, "effective: sound.%s = (silent)", names[i]);
	}
}

static void play_sound(enum snd_event ev) {
	if (!cfg.sound_enabled || !cfg.sound_cmd[ev]) return;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	long ms = (now.tv_sec - snd.last.tv_sec) * 1000 + (now.tv_nsec - snd.last.tv_nsec) / 1000000;
	if (snd.last.tv_sec && ms < cfg.sound_cooldown_ms) return;
	if (snd.pid > 0) {
		if (cfg.sound_overlap == OVERLAP_DROP) return;
		if (cfg.sound_overlap == OVERLAP_RESTART) kill(snd.pid, SIGTERM);
	}
	snd.last = now;
	pid_t p = spawn_argv(cfg.sound_cmd[ev], true);
	wlr_log(WLR_DEBUG, "sound event %d: %s (pid %d)", (int)ev, cfg.sound_cmd[ev][0], (int)p);
	if (p > 0) snd.pid = p;
}

/* ------------------------------------------------------------------ */
/* slot helpers                                                        */
/* ------------------------------------------------------------------ */

static int slot_count(void) { return wl_list_length(&S.slots); }

static int slot_index(struct slot *s) {
	int i = 0;
	struct slot *it;
	wl_list_for_each(it, &S.slots, link) {
		if (it == s) return i;
		i++;
	}
	return -1;
}

static struct slot *slot_at(int idx) {
	if (idx < 0) return NULL;
	int i = 0;
	struct slot *it;
	wl_list_for_each(it, &S.slots, link) {
		if (i++ == idx) return it;
	}
	return NULL;
}

static void swap_slots(struct slot *a, struct slot *b) {
	if (a == b) return;
	struct wl_list *pa = a->link.prev, *pb = b->link.prev;
	if (pa == &b->link) { /* b directly before a */
		wl_list_remove(&a->link);
		wl_list_insert(b->link.prev, &a->link);
		return;
	}
	if (pb == &a->link) { /* a directly before b */
		wl_list_remove(&b->link);
		wl_list_insert(a->link.prev, &b->link);
		return;
	}
	wl_list_remove(&a->link);
	wl_list_remove(&b->link);
	wl_list_insert(pa, &b->link);
	wl_list_insert(pb, &a->link);
}

static struct view *view_from_toplevel(struct wlr_xdg_toplevel *t) {
	if (!t || !t->base->data) return NULL;
	struct wlr_scene_tree *tree = t->base->data;
	struct view *v = tree->node.data;
	return (v && v->kind == K_VIEW) ? v : NULL;
}

/* ------------------------------------------------------------------ */
/* layout                                                              */
/* ------------------------------------------------------------------ */

static void view_configure(struct view *v, int w, int h) {
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	if (w != v->cur_w || h != v->cur_h) {
		wlr_xdg_toplevel_set_size(v->xdg_toplevel, w, h);
		v->cur_w = w;
		v->cur_h = h;
	}
}

static void slot_layout_views(struct slot *s, int fw, int fh) {
	if (!s->windows_mode) {
		struct view *v;
		wl_list_for_each(v, &s->views, link) {
			bool main = (v == s->focus_view);
			wlr_scene_node_set_enabled(&v->scene_tree->node, main);
			if (!main) continue;
			wlr_scene_node_set_position(&v->scene_tree->node, 0, 0);
			view_configure(v, fw, fh);
		}
		return;
	}
	int n = 0, placed = 0;
	struct view *v;
	/* pass 1: ordinary windows. In a slot that a dialog flipped to windows mode
	 * automatically, the first app window keeps the whole slot so the app does
	 * not shrink just because it opened a dialog. */
	wl_list_for_each(v, &s->views, link) {
		if (v->xdg_toplevel->parent && view_from_toplevel(v->xdg_toplevel->parent)) continue;
		if (!v->float_w) {
			if (s->auto_wm && placed == 0) {
				v->float_w = fw; v->float_h = fh; v->float_x = 0; v->float_y = 0;
			} else {
				v->float_w = (int)(cfg.default_w * fw);
				v->float_h = (int)(cfg.default_h * fh);
				v->float_x = cfg.wm_margin + 32 * n;
				v->float_y = cfg.wm_margin + 32 * n;
			}
		}
		placed++;
		wlr_scene_node_set_enabled(&v->scene_tree->node, true);
		wlr_scene_node_set_position(&v->scene_tree->node, v->float_x, v->float_y);
		view_configure(v, v->float_w, v->float_h);
		n++;
	}
	/* pass 2: dialogs (windows with a parent) keep their own size and open centred over the parent */
	wl_list_for_each(v, &s->views, link) {
		struct view *pv = v->xdg_toplevel->parent ? view_from_toplevel(v->xdg_toplevel->parent) : NULL;
		if (!pv) continue;
		if (!v->float_w) {
			struct wlr_box g = v->xdg_toplevel->base->geometry;
			int w = g.width > 0 ? g.width : (int)(cfg.default_w * fw);
			int h = g.height > 0 ? g.height : (int)(cfg.default_h * fh);
			if (w > fw) w = fw;
			if (h > fh) h = fh;
			int px = pv->slot == s ? pv->float_x : 0, py = pv->slot == s ? pv->float_y : 0;
			int pw = pv->slot == s && pv->float_w ? pv->float_w : fw, ph = pv->slot == s && pv->float_h ? pv->float_h : fh;
			int x = px + (pw - w) / 2, y = py + (ph - h) / 2;
			if (x + w > fw) x = fw - w;
			if (y + h > fh) y = fh - h;
			if (x < 0) x = 0;
			if (y < 0) y = 0;
			v->float_w = w; v->float_h = h; v->float_x = x; v->float_y = y;
		}
		wlr_scene_node_set_enabled(&v->scene_tree->node, true);
		wlr_scene_node_set_position(&v->scene_tree->node, v->float_x, v->float_y);
		view_configure(v, v->float_w, v->float_h);
	}
}

/* ------------------------------------------------------------------ */
/* neighbour look (tilt / scale / dim) and the slot-switch animation   */
/* ------------------------------------------------------------------ */

struct geom { int ow, oh, fx, fw, ay, ah, cy, peekw; };

static bool compute_geom(struct geom *g) {
	if (!S.output) return false;
	int ow = S.output->w, oh = S.output->h;
	struct wlr_box u = S.usable;
	if (u.width <= 0 || u.height <= 0) u = (struct wlr_box){ 0, 0, ow, oh };
	int ml = cfg.margin_left + cfg.edge_gap, mr = cfg.margin_right + cfg.edge_gap;
	int mt = cfg.margin_top + (cfg.edge_gap_top >= 0 ? cfg.edge_gap_top : cfg.edge_gap);
	int mb = cfg.margin_bottom + (cfg.edge_gap_bottom >= 0 ? cfg.edge_gap_bottom : cfg.edge_gap);
	int ax = u.x + ml, ay = u.y + mt;
	int aw = u.width - ml - mr;
	int ah = u.height - mt - mb;
	if (aw < 1) aw = 1;
	if (ah < 1) ah = 1;
	int fw = (int)(cfg.focus_width * (float)ow);
	if (fw > aw) fw = aw;
	g->ow = ow; g->oh = oh; g->fw = fw; g->fx = ax + (aw - fw) / 2;
	g->ay = ay; g->ah = ah; g->cy = ay + ah / 2;
	g->peekw = (int)(cfg.peek * (float)ow);
	return true;
}

static int64_t now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static bool look_on(void) {
	return !S.img_failed && (cfg.look_tilt != 0.0f || cfg.look_scale != 1.0f || cfg.look_dim != 0.0f);
}

static bool anim_on(void) {
	return look_on() && cfg.anim_enabled && !cfg.reduced_motion && cfg.anim_ms > 0;
}

/* cubic-bezier(x1,y1,x2,y2) like CSS: solve x(u) = t, return y(u) */
static double bezier_ease(double t) {
	double x1 = cfg.anim_bez[0], y1 = cfg.anim_bez[1], x2 = cfg.anim_bez[2], y2 = cfg.anim_bez[3];
	double lo = 0.0, hi = 1.0, u = t;
	for (int i = 0; i < 24; i++) {
		double m = 1.0 - u;
		double x = 3 * m * m * u * x1 + 3 * m * u * u * x2 + u * u * u;
		if (fabs(x - t) < 1e-6) break;
		if (x < t) lo = u; else hi = u;
		u = (lo + hi) / 2.0;
	}
	double m = 1.0 - u;
	return 3 * m * m * u * y1 + 3 * m * u * u * y2 + u * u * u;
}

/* every curve starts at full speed and slows toward the end */
static double ease(double t) {
	if (t < 0) t = 0;
	if (t > 1) t = 1;
	double u = 1.0 - t;
	switch (cfg.anim_easing) {
	case EASE_CUBIC_OUT: return 1.0 - u * u * u;
	case EASE_QUAD_OUT: return 1.0 - u * u;
	case EASE_QUART_OUT: return 1.0 - u * u * u * u;
	case EASE_QUINT_OUT: return 1.0 - u * u * u * u * u;
	case EASE_EXPO_OUT: return t >= 1.0 ? 1.0 : 1.0 - pow(2.0, -10.0 * t);
	case EASE_LINEAR: return t;
	case EASE_BEZIER: return bezier_ease(t);
	default: return sin(t * M_PI / 2.0);
	}
}

/* forward declaration: anim_retarget() takes the first step at once */
static void anim_step(void);

static double cur_pos(void) {
	int n = slot_count();
	double p = S.have_pos ? S.pos : (S.focused ? slot_index(S.focused) : 0);
	if (n > 0 && p > n - 1) p = n - 1;
	if (p < 0) p = 0;
	return p;
}

static void anim_snap(void) {
	if (S.focused) { S.pos = slot_index(S.focused); S.have_pos = true; }
	S.animating = false;
}

static void anim_retarget(int idx, bool animate) {
	if (!S.have_pos || !animate || !anim_on() || fabs(S.pos - idx) < 0.001) {
		S.pos = idx;
		S.have_pos = true;
		S.animating = false;
		return;
	}
	S.anim_from = S.pos;
	S.anim_to = idx;
	/* Start a few ms "into" the move and draw that frame right away: otherwise the
	 * key press shows one unchanged frame and the start feels soft instead of instant. */
	S.anim_t0 = now_ms() - 10;
	S.animating = true;
	anim_step();
	if (S.output) wlr_output_schedule_frame(S.output->wlr_output);
}

/* wlr_swapchain_acquire() lost its age argument in 0.19 */
#if WLR_VERSION_NUM >= ((0 << 16) | (19 << 8))
#define SWAPCHAIN_ACQUIRE(sc) wlr_swapchain_acquire(sc)
#else
#define SWAPCHAIN_ACQUIRE(sc) wlr_swapchain_acquire(sc, NULL)
#endif

/* The output's own swapchain format is known to be drawable by this renderer
 * (wlroots has no public "render formats" accessor), so reuse its modifiers, with an alpha channel. */
static bool swapchain_ready(void) {
	return S.output && S.output->wlr_output->swapchain;
}

static struct wlr_swapchain *make_swapchain(int w, int h) {
	if (!swapchain_ready()) return NULL;
	struct wlr_drm_format fmt = S.output->wlr_output->swapchain->format;
	fmt.format = DRM_FORMAT_ARGB8888;
	return wlr_swapchain_create(S.allocator, w, h, &fmt);
}

static void slot_img_release(struct slot *s) {
	if (s->img) {
		wlr_scene_buffer_set_buffer(s->img, NULL);
		wlr_scene_node_set_enabled(&s->img->node, false);
	}
	if (s->flat_sc) { wlr_swapchain_destroy(s->flat_sc); s->flat_sc = NULL; }
	if (s->out_sc) { wlr_swapchain_destroy(s->out_sc); s->out_sc = NULL; }
	free(s->col_t);
	free(s->col_b);
	s->col_t = s->col_b = NULL;
	s->img_active = false;
}

/* only the solid part of the picture takes clicks, not its transparent surroundings */
static bool img_accepts_input(struct wlr_scene_buffer *buffer, double *sx, double *sy) {
	struct img_tag *t = buffer->node.data;
	if (!t || !t->slot) return false;
	struct slot *s = t->slot;
	int x = (int)*sx, y = (int)*sy;
	if (!s->col_t || x < 0 || x >= s->out_w) return false;
	return y >= s->col_t[x] && y < s->col_b[x];
}

struct draw_ctx {
	struct wlr_render_pass *pass;
	int ox, oy;
	struct timespec now;
};

static void draw_surface_iter(struct wlr_surface *surface, int sx, int sy, void *data) {
	struct draw_ctx *c = data;
	wlr_surface_send_frame_done(surface, &c->now); /* keeps apps in neighbour slots running */
	struct wlr_texture *tex = wlr_surface_get_texture(surface);
	if (!tex) return;
	struct wlr_fbox src;
	wlr_surface_get_buffer_source_box(surface, &src);
	wlr_render_pass_add_texture(c->pass, &(struct wlr_render_texture_options){
		.texture = tex,
		.src_box = src,
		.dst_box = { c->ox + sx, c->oy + sy, surface->current.width, surface->current.height },
		.filter_mode = WLR_SCALE_FILTER_BILINEAR,
	});
}

/* window geometry: a getter up to 0.18, a plain struct member from 0.19 */
static struct wlr_box xdg_geometry(struct wlr_xdg_surface *base) {
#if WLR_VERSION_NUM >= ((0 << 16) | (19 << 8))
	return base->geometry;
#else
	struct wlr_box geo;
	wlr_xdg_surface_get_geometry(base, &geo);
	return geo;
#endif
}

static void draw_view(struct draw_ctx *c, struct view *v, bool floating) {
	struct wlr_box geo = xdg_geometry(v->xdg_toplevel->base);
	c->ox = (floating ? v->float_x : 0) - geo.x;
	c->oy = (floating ? v->float_y : 0) - geo.y;
	wlr_xdg_surface_for_each_surface(v->xdg_toplevel->base, draw_surface_iter, c);
}

struct proj { double oc, D, xar, costh, sinth; int side; };

/* inverse of the perspective projection: which point (distance d from the anchor
 * edge, measured along the tilted slot) ends up in screen column xs */
static bool proj_d(const struct proj *p, double xs, double *d) {
	double q = (xs - p->oc) / p->D;
	double den = p->side * p->costh + q * p->sinth;
	if (fabs(den) < 1e-6) return false;
	*d = (q * p->D - p->xar) / den;
	return true;
}

/* Draw slot s as a tilted, shrunk, dimmed picture. o = slot offset from the strip
 * position (negative = left of the focused slot). Returns false on a rendering failure. */
static bool render_slot_image(struct slot *s, double o, const struct geom *g, const struct timespec *now) {
	if (!swapchain_ready()) { /* the output has not committed a frame yet: try again next frame */
		S.img_dirty = true;
		if (S.output) wlr_output_schedule_frame(S.output->wlr_output);
		return true;
	}
	double ao = fabs(o), t = ao > 1.0 ? 1.0 : ao;
	int side = o < 0 ? -1 : 1;
	double th = (double)cfg.look_tilt * M_PI / 180.0 * t;
	double sc = 1.0 + ((double)cfg.look_scale - 1.0) * t;
	double dim = (double)cfg.look_dim * t;
	double W = g->fw * sc, H = g->ah * sc;

	/* anchor edge = the edge facing the focused slot; the slot hangs off it, turned by th */
	double xa;
	if (side < 0) {
		double xs = g->peekw;
		if (xs > g->fx - cfg.gap) xs = g->fx - cfg.gap;
		double x0 = g->fx + g->fw;
		xa = ao > 1.0 ? xs - (ao - 1.0) * g->ow * 0.5 : x0 + (xs - x0) * t;
	} else {
		double xs = g->ow - g->peekw;
		if (xs < g->fx + g->fw + cfg.gap) xs = g->fx + g->fw + cfg.gap;
		double x0 = g->fx;
		xa = ao > 1.0 ? xs + (ao - 1.0) * g->ow * 0.5 : x0 + (xs - x0) * t;
	}
	struct proj p = { .oc = g->ow / 2.0, .D = (double)cfg.look_persp * g->ow, .costh = cos(th), .sinth = sin(th), .side = side };
	p.xar = xa - p.oc;

	/* screen columns the slot covers */
	double xa_s = p.oc + p.xar;
	double dd = W;
	if (p.D - dd * p.sinth < 1.0) return true; /* absurd settings: draw nothing */
	double xo_s = p.oc + (p.xar + side * dd * p.costh) * p.D / (p.D - dd * p.sinth);
	int c0 = (int)floor(fmin(xa_s, xo_s)), c1 = (int)ceil(fmax(xa_s, xo_s));
	if (c0 < 0) c0 = 0;
	if (c1 > g->ow) c1 = g->ow;
	if (c1 <= c0) { slot_img_release(s); return true; }

	/* 1. the slot as it looks flat */
	if (s->flat_sc && (s->flat_w != g->fw || s->flat_h != g->ah)) { wlr_swapchain_destroy(s->flat_sc); s->flat_sc = NULL; }
	if (!s->flat_sc) {
		s->flat_sc = make_swapchain(g->fw, g->ah);
		if (!s->flat_sc) return false;
		s->flat_w = g->fw;
		s->flat_h = g->ah;
	}
	struct wlr_buffer *fb = SWAPCHAIN_ACQUIRE(s->flat_sc);
	if (!fb) return false;
	struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(S.renderer, fb, NULL);
	if (!pass) { wlr_buffer_unlock(fb); return false; }
	wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
		.box = { 0, 0, g->fw, g->ah }, .color = { 0, 0, 0, 0 }, .blend_mode = WLR_RENDER_BLEND_MODE_NONE });
	struct draw_ctx ctx = { .pass = pass, .now = *now };
	struct view *v;
	wl_list_for_each_reverse(v, &s->views, link) { /* oldest first */
		if (!v->mapped || v == s->focus_view) continue;
		if (s->windows_mode) draw_view(&ctx, v, true);
	}
	if (s->focus_view && s->focus_view->mapped) draw_view(&ctx, s->focus_view, s->windows_mode);
	if (!wlr_render_pass_submit(pass)) { wlr_buffer_unlock(fb); return false; }
	struct wlr_texture *tex = wlr_texture_from_buffer(S.renderer, fb);
	if (!tex) { wlr_buffer_unlock(fb); return false; }

	/* 2. squeeze it, column by column, into the output-sized picture */
	if (s->out_sc && (s->out_w != g->ow || s->out_h != g->oh)) {
		wlr_swapchain_destroy(s->out_sc);
		s->out_sc = NULL;
		free(s->col_t); free(s->col_b);
		s->col_t = s->col_b = NULL;
	}
	if (!s->out_sc) {
		s->out_sc = make_swapchain(g->ow, g->oh);
		s->col_t = calloc((size_t)g->ow, sizeof *s->col_t);
		s->col_b = calloc((size_t)g->ow, sizeof *s->col_b);
		if (!s->out_sc || !s->col_t || !s->col_b) { wlr_texture_destroy(tex); wlr_buffer_unlock(fb); return false; }
		s->out_w = g->ow;
		s->out_h = g->oh;
	}
	memset(s->col_t, 0, (size_t)g->ow * sizeof *s->col_t);
	memset(s->col_b, 0, (size_t)g->ow * sizeof *s->col_b);

	struct wlr_buffer *ob = SWAPCHAIN_ACQUIRE(s->out_sc);
	struct wlr_render_pass *p2 = ob ? wlr_renderer_begin_buffer_pass(S.renderer, ob, NULL) : NULL;
	if (!p2) { if (ob) wlr_buffer_unlock(ob); wlr_texture_destroy(tex); wlr_buffer_unlock(fb); return false; }
	wlr_render_pass_add_rect(p2, &(struct wlr_render_rect_options){
		.box = { 0, 0, g->ow, g->oh }, .color = { 0, 0, 0, 0 }, .blend_mode = WLR_RENDER_BLEND_MODE_NONE });

	int step = (c1 - c0) > 700 ? 2 : 1;
	for (int x = c0; x < c1; x += step) {
		int w = (x + step > c1) ? c1 - x : step;
		double dA, dB, dC;
		if (!proj_d(&p, x, &dA) || !proj_d(&p, x + w, &dB) || !proj_d(&p, x + w * 0.5, &dC)) continue;
		if (dC < 0 || dC > W) continue;
		dA = dA < 0 ? 0 : dA > W ? W : dA;
		dB = dB < 0 ? 0 : dB > W ? W : dB;
		double uA = dA / sc, uB = dB / sc; /* back to unscaled slot pixels */
		double sA = side < 0 ? g->fw - uA : uA, sB = side < 0 ? g->fw - uB : uB;
		double sx0 = fmin(sA, sB), sw = fabs(sB - sA);
		if (sw < 0.25) sw = 0.25;
		if (sx0 + sw > g->fw) sx0 = g->fw - sw;
		if (sx0 < 0) sx0 = 0;
		double sdepth = p.D / (p.D - dC * p.sinth);
		double hh = H * sdepth;
		int y = (int)lround(g->cy - hh / 2.0), h = (int)lround(hh);
		if (h < 1) continue;
		struct wlr_box dst = { x, y, w, h };
		wlr_render_pass_add_texture(p2, &(struct wlr_render_texture_options){
			.texture = tex,
			.src_box = { sx0, 0, sw, g->ah },
			.dst_box = dst,
			.filter_mode = WLR_SCALE_FILTER_BILINEAR,
		});
		if (dim > 0.001)
			wlr_render_pass_add_rect(p2, &(struct wlr_render_rect_options){
				.box = dst, .color = { 0, 0, 0, (float)dim } });
		int yt = y < 0 ? 0 : y, yb = y + h > g->oh ? g->oh : y + h;
		for (int k = x; k < x + w && k < g->ow; k++) { s->col_t[k] = (int16_t)yt; s->col_b[k] = (int16_t)yb; }
	}
	bool ok = wlr_render_pass_submit(p2);
	wlr_texture_destroy(tex);
	wlr_buffer_unlock(fb);
	if (!ok) { wlr_buffer_unlock(ob); return false; }

	wlr_scene_buffer_set_buffer(s->img, ob);
	wlr_buffer_unlock(ob);
	wlr_scene_node_set_position(&s->img->node, 0, 0);
	wlr_scene_node_set_enabled(&s->img->node, true);
	s->img_active = true;
	return true;
}

static void relayout(void);

static void update_images(void) {
	struct geom g;
	if (!compute_geom(&g)) return;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	bool look = look_on();
	double pos = cur_pos();
	struct act_ent { struct slot *s; double ao; } act[64];
	int na = 0, i = 0;
	struct slot *s;
	wl_list_for_each(s, &S.slots, link) {
		double o = i++ - pos;
		if (!look || fabs(o) < 0.001 || fabs(o) >= 2.0) {
			if (s->img_active) slot_img_release(s);
			continue;
		}
		if (!render_slot_image(s, o, &g, &now)) {
			wlr_log(WLR_ERROR, "tilted neighbour rendering failed, falling back to flat neighbours");
			S.img_failed = true;
			struct slot *r;
			wl_list_for_each(r, &S.slots, link) slot_img_release(r);
			relayout();
			return;
		}
		if (s->img_active && na < 64) { act[na].s = s; act[na].ao = fabs(o); na++; }
	}
	/* nearest picture on top: raise farthest first */
	for (int a = 0; a < na; a++)
		for (int b = a + 1; b < na; b++)
			if (act[b].ao > act[a].ao) { struct act_ent tmp = act[a]; act[a] = act[b]; act[b] = tmp; }
	for (int a = 0; a < na; a++) wlr_scene_node_raise_to_top(&act[a].s->img->node);
}

static void anim_step(void) {
	double t = cfg.anim_ms > 0 ? (double)(now_ms() - S.anim_t0) / cfg.anim_ms : 1.0;
	if (t >= 1.0) {
		S.pos = S.anim_to;
		S.animating = false;
	} else {
		double j = cfg.anim_jump; /* part of the move taken instantly, the rest follows the curve */
		S.pos = S.anim_from + (S.anim_to - S.anim_from) * (j + (1.0 - j) * ease(t));
	}
	relayout();
}

static int img_timer_cb(void *data) {
	(void)data;
	/* neighbours are pictures: refresh them a few times a second even when idle,
	 * so apps that draw into subsurfaces keep updating in the background */
	if (S.output && look_on() && !S.animating) {
		struct slot *s;
		wl_list_for_each(s, &S.slots, link) {
			if (!s->img_active) continue;
			S.img_dirty = true;
			wlr_output_schedule_frame(S.output->wlr_output);
			break;
		}
	}
	wl_event_source_timer_update(S.img_timer, 100);
	return 0;
}

static void relayout(void) {
	struct geom g;
	if (!compute_geom(&g)) return;
	wlr_scene_rect_set_size(S.bg, g.ow, g.oh);
	wlr_scene_rect_set_color(S.bg, cfg.bg_color);

	bool look = look_on();
	double pos = cur_pos();
	int fidx = S.focused ? slot_index(S.focused) : 0;
	int i = 0;
	struct slot *s;
	wl_list_for_each(s, &S.slots, link) {
		if (look) {
			/* settled: the focused slot is the live window; everything else is a picture */
			bool live = fabs(i - pos) < 0.001;
			wlr_scene_node_set_position(&s->tree->node, g.fx, g.ay);
			wlr_scene_node_set_enabled(&s->tree->node, live);
			wlr_scene_node_set_position(&s->ftree->node, g.fx, g.ay);
			wlr_scene_node_set_enabled(&s->ftree->node, live);
			slot_layout_views(s, g.fw, g.ah);
		} else {
			/* flat neighbours, hard cuts */
			int d = i - fidx, x;
			if (d == 0) x = g.fx;
			else if (d == -1) { x = g.peekw - g.fw; if (x > g.fx - cfg.gap - g.fw) x = g.fx - cfg.gap - g.fw; }
			else if (d == 1) { x = g.ow - g.peekw; if (x < g.fx + g.fw + cfg.gap) x = g.fx + g.fw + cfg.gap; }
			else x = d < 0 ? -g.fw - 100 : g.ow + 100;
			bool vis = d >= -1 && d <= 1;
			wlr_scene_node_set_position(&s->tree->node, x, g.ay);
			wlr_scene_node_set_enabled(&s->tree->node, vis);
			wlr_scene_node_set_position(&s->ftree->node, x, g.ay);
			wlr_scene_node_set_enabled(&s->ftree->node, vis);
			slot_layout_views(s, g.fw, g.ah);
		}
		i++;
	}
	if (S.focused) { /* focused slot draws above its neighbours, in both layers */
		wlr_scene_node_raise_to_top(&S.focused->tree->node);
		wlr_scene_node_raise_to_top(&S.focused->ftree->node);
	}
	update_images();
}

static void arrange_layers(void) {
	if (!S.output) return;
	struct wlr_box full = { 0, 0, S.output->w, S.output->h };
	struct wlr_box usable = full;
	for (int pass = 0; pass < 2; pass++) {
		struct layer *l;
		wl_list_for_each(l, &S.layers, link) {
			if (!l->ls->initialized) continue;
			bool excl = l->ls->current.exclusive_zone > 0;
			if ((pass == 0) != excl) continue;
			wlr_scene_layer_surface_v1_configure(l->scene, &full, &usable);
		}
	}
	S.usable = usable;
	relayout();
}

/* ------------------------------------------------------------------ */
/* focus                                                               */
/* ------------------------------------------------------------------ */

static void keyboard_enter(struct wlr_surface *surface) {
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(S.seat);
	if (kb)
		wlr_seat_keyboard_notify_enter(S.seat, surface, kb->keycodes, kb->num_keycodes, &kb->modifiers);
	else
		wlr_seat_keyboard_notify_enter(S.seat, surface, NULL, 0, NULL);
}

static void focus_view(struct view *v) {
	if (!v) {
		if (!S.focused_layer) wlr_seat_keyboard_notify_clear_focus(S.seat);
		return;
	}
	v->slot->focus_view = v;
	wlr_scene_node_raise_to_top(&v->scene_tree->node);
	if (S.active_view && S.active_view != v)
		wlr_xdg_toplevel_set_activated(S.active_view->xdg_toplevel, false);
	S.active_view = v;
	wlr_xdg_toplevel_set_activated(v->xdg_toplevel, true);
	if (!S.focused_layer) keyboard_enter(v->xdg_toplevel->base->surface);
}

static void focus_slot(struct slot *s, bool play) {
	if (S.focused && S.focused != s && S.focused != NULL) S.last_slot = S.focused;
	bool changed = (S.focused != s);
	S.focused = s;
	if (s) anim_retarget(slot_index(s), changed);
	if (s && s->focus_view) focus_view(s->focus_view);
	else if (!s) {
		if (S.active_view) wlr_xdg_toplevel_set_activated(S.active_view->xdg_toplevel, false);
		S.active_view = NULL;
		if (!S.focused_layer) wlr_seat_keyboard_notify_clear_focus(S.seat);
	}
	relayout();
	if (changed && play && s) play_sound(SND_SLOT_SWITCH);
}

static struct slot *slot_create(bool play) {
	struct slot *s = calloc(1, sizeof *s);
	if (!s) abort();
	wl_list_init(&s->views);
	s->tree = wlr_scene_tree_create(S.t_strip);
	s->ftree = wlr_scene_tree_create(S.t_float);
	s->tag.kind = K_SLOT;
	s->tag.slot = s;
	s->img = wlr_scene_buffer_create(S.t_side, NULL);
	s->img->node.data = &s->tag;
	s->img->point_accepts_input = img_accepts_input;
	wlr_scene_node_set_enabled(&s->img->node, false);
	if (cfg.new_slot_at_right) {
		wl_list_insert(S.slots.prev, &s->link);
	} else {
		wl_list_insert(&S.slots, &s->link);
		if (S.have_pos) { /* everything shifts right by one */
			S.pos += 1; S.anim_from += 1; S.anim_to += 1;
		}
	}
	if (play) play_sound(SND_SLOT_OPEN);
	return s;
}

static void slot_destroy(struct slot *s) {
	int idx = slot_index(s);
	bool was_focused = (S.focused == s);
	struct slot *next = NULL;
	if (was_focused) {
		struct slot *l = slot_at(idx - 1), *r = slot_at(idx + 1);
		switch (cfg.close_focus) {
		case CLOSE_RIGHT: next = r ? r : l; break;
		case CLOSE_RECENT: next = (S.last_slot && S.last_slot != s) ? S.last_slot : (l ? l : r); break;
		default: next = l ? l : r; break;
		}
	}
	wl_list_remove(&s->link);
	slot_img_release(s);
	wlr_scene_node_destroy(&s->img->node);
	wlr_scene_node_destroy(&s->tree->node);
	wlr_scene_node_destroy(&s->ftree->node);
	if (S.have_pos) {
		if (idx < S.pos) S.pos -= 1;
		S.animating = false;
		int n = slot_count();
		if (n == 0) S.have_pos = false;
		else if (S.pos > n - 1) S.pos = n - 1;
	}
	if (S.last_slot == s) S.last_slot = NULL;
	if (S.focused == s) S.focused = NULL;
	free(s);
	play_sound(SND_SLOT_CLOSE);
	if (was_focused) focus_slot(next, false);
	else relayout();
}

/* Flip a slot between strip mode and windows mode. Windows-mode windows live in
 * the floating layer, which is always stacked above every strip-mode window. */
static void slot_set_windows_mode(struct slot *s, bool on) {
	s->windows_mode = on;
	struct view *v;
	wl_list_for_each(v, &s->views, link) {
		wlr_scene_node_reparent(&v->scene_tree->node, on ? s->ftree : s->tree);
		v->cur_w = v->cur_h = 0; /* force the size to be re-sent */
	}
}

/* ------------------------------------------------------------------ */
/* xdg toplevels                                                       */
/* ------------------------------------------------------------------ */

static struct slot *slot_of_client(struct wl_client *client, struct view *self) {
	struct slot *best = NULL, *s;
	wl_list_for_each(s, &S.slots, link) {
		struct view *v;
		wl_list_for_each(v, &s->views, link) {
			if (v == self || !v->mapped) continue;
			if (wl_resource_get_client(v->xdg_toplevel->base->resource) != client) continue;
			if (s == S.focused) return s;
			if (!best) best = s;
		}
	}
	return best;
}

static bool rule_new_slot(const char *app) {
	for (size_t i = 0; i < cfg.n_rules; i++)
		if (cfg.rules[i].new_slot && fnmatch(cfg.rules[i].app_id, app, 0) == 0) return true;
	return false;
}

static void view_map(struct wl_listener *l, void *data) {
	(void)data;
	struct view *v = wl_container_of(l, v, map);
	struct wlr_xdg_toplevel *t = v->xdg_toplevel;
	v->mapped = true;

	/* Windows that belong to an app already in a slot (dialogs, extra windows)
	 * join that slot instead of opening their own. */
	struct slot *target = NULL;
	const char *app0 = t->app_id ? t->app_id : "";
	if (cfg.popups_auto) {
		struct view *pv = view_from_toplevel(t->parent);
		if (pv && pv->slot) target = pv->slot;
		else if (cfg.same_client_join && !rule_new_slot(app0))
			target = slot_of_client(wl_resource_get_client(t->base->resource), v);
	}

	if (target) {
		v->slot = target;
		wl_list_insert(&target->views, &v->link);
		if (!target->windows_mode) {
			slot_set_windows_mode(target, true);
			target->auto_wm = true;
			play_sound(SND_WINDOWS_MODE);
		} else {
			wlr_scene_node_reparent(&v->scene_tree->node, target->ftree);
		}
		focus_view(v);
		if (S.focused != target) focus_slot(target, true);
		else relayout();
		return;
	}

	struct slot *s = slot_create(true);
	v->slot = s;
	wlr_scene_node_reparent(&v->scene_tree->node, s->tree);
	wl_list_insert(&s->views, &v->link);
	s->focus_view = v;
	const char *app = t->app_id ? t->app_id : "";
	for (size_t i = 0; i < cfg.n_rules; i++) {
		if (fnmatch(cfg.rules[i].app_id, app, 0) == 0 && cfg.rules[i].windows_mode) {
			slot_set_windows_mode(s, true);
			break;
		}
	}
	focus_slot(s, false);
}

static void view_unmap(struct wl_listener *l, void *data) {
	(void)data;
	struct view *v = wl_container_of(l, v, unmap);
	v->mapped = false;
	if (grab.view == v) { grab.mode = GRAB_NONE; grab.view = NULL; }
	struct slot *s = v->slot;
	if (!s) return;
	wl_list_remove(&v->link);
	v->slot = NULL;
	wlr_scene_node_reparent(&v->scene_tree->node, S.t_hidden);
	if (S.active_view == v) S.active_view = NULL;
	if (s->focus_view == v)
		s->focus_view = wl_list_empty(&s->views) ? NULL : wl_container_of(s->views.next, v, link);

	if (wl_list_empty(&s->views)) {
		slot_destroy(s);
		return;
	}
	if (s->windows_mode && s->auto_wm && cfg.revert_when_single && wl_list_length(&s->views) <= 1) {
		slot_set_windows_mode(s, false);
		s->auto_wm = false;
		play_sound(SND_WINDOWS_MODE);
	}
	if (S.focused == s) focus_view(s->focus_view);
	relayout();
}

static void view_commit(struct wl_listener *l, void *data) {
	(void)data;
	struct view *v = wl_container_of(l, v, commit);
	if (v->xdg_toplevel->base->initial_commit)
		wlr_xdg_toplevel_set_size(v->xdg_toplevel, 0, 0);
	if (v->slot && v->slot->img_active) { /* a neighbour's picture is stale */
		S.img_dirty = true;
		if (S.output) wlr_output_schedule_frame(S.output->wlr_output);
	}
}

static void view_destroy(struct wl_listener *l, void *data) {
	(void)data;
	struct view *v = wl_container_of(l, v, destroy);
	if (grab.view == v) { grab.mode = GRAB_NONE; grab.view = NULL; }
	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->commit.link);
	wl_list_remove(&v->destroy.link);
	wl_list_remove(&v->request_move.link);
	wl_list_remove(&v->request_resize.link);
	wl_list_remove(&v->request_maximize.link);
	wl_list_remove(&v->request_fullscreen.link);
	free(v);
}

/* Maximize/fullscreen are not implemented yet: answer with the current state so
 * the client does not wait. Each request has its own handler because
 * wl_container_of() needs the exact listener member the signal came from. */
static void view_request_maximize(struct wl_listener *l, void *data) {
	(void)data;
	struct view *v = wl_container_of(l, v, request_maximize);
	if (v->xdg_toplevel->base->initialized)
		wlr_xdg_surface_schedule_configure(v->xdg_toplevel->base);
}

static void view_request_fullscreen(struct wl_listener *l, void *data) {
	(void)data;
	struct view *v = wl_container_of(l, v, request_fullscreen);
	if (v->xdg_toplevel->base->initialized)
		wlr_xdg_surface_schedule_configure(v->xdg_toplevel->base);
}

static uint32_t current_mods(void) {
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(S.seat);
	return kb ? (wlr_keyboard_get_modifiers(kb) & MODS_MASK) : 0;
}

static void grab_end(void) {
	if (grab.mode == GRAB_NONE) return;
	grab.mode = GRAB_NONE;
	grab.view = NULL;
	wlr_cursor_set_xcursor(S.cursor, S.cursor_mgr, "default");
}

static bool grab_begin(struct view *v, enum grab_mode mode, uint32_t edges) {
	if (!v || !v->slot || !v->slot->windows_mode || v->slot != S.focused || !v->float_w)
		return false;
	grab.mode = mode;
	grab.view = v;
	grab.edges = edges;
	grab.cx = S.cursor->x;
	grab.cy = S.cursor->y;
	grab.x = v->float_x;
	grab.y = v->float_y;
	grab.w = v->float_w;
	grab.h = v->float_h;
	focus_view(v);
	wlr_seat_pointer_clear_focus(S.seat);
	wlr_cursor_set_xcursor(S.cursor, S.cursor_mgr,
		mode == GRAB_MOVE ? "grabbing" : wlr_xcursor_get_resize_name(edges));
	return true;
}

static void grab_motion(void) {
	struct view *v = grab.view;
	if (!v) return;
	int dx = (int)(S.cursor->x - grab.cx), dy = (int)(S.cursor->y - grab.cy);
	if (grab.mode == GRAB_MOVE) {
		v->float_x = grab.x + dx;
		v->float_y = grab.y + dy;
	} else {
		int x = grab.x, y = grab.y, w = grab.w, h = grab.h;
		if (grab.edges & WLR_EDGE_LEFT) { x += dx; w -= dx; }
		if (grab.edges & WLR_EDGE_RIGHT) w += dx;
		if (grab.edges & WLR_EDGE_TOP) { y += dy; h -= dy; }
		if (grab.edges & WLR_EDGE_BOTTOM) h += dy;
		if (w < MIN_WIN_W) { if (grab.edges & WLR_EDGE_LEFT) x -= MIN_WIN_W - w; w = MIN_WIN_W; }
		if (h < MIN_WIN_H) { if (grab.edges & WLR_EDGE_TOP) y -= MIN_WIN_H - h; h = MIN_WIN_H; }
		v->float_x = x; v->float_y = y; v->float_w = w; v->float_h = h;
		view_configure(v, w, h);
	}
	wlr_scene_node_set_position(&v->scene_tree->node, v->float_x, v->float_y);
}

/* client-initiated (title bar drag / edge drag) requests */
static void view_request_move(struct wl_listener *l, void *data) {
	(void)data;
	struct view *v = wl_container_of(l, v, request_move);
	if (S.seat->pointer_state.focused_surface == v->xdg_toplevel->base->surface
			&& S.seat->pointer_state.button_count > 0)
		grab_begin(v, GRAB_MOVE, 0);
}

static void view_request_resize(struct wl_listener *l, void *data) {
	struct wlr_xdg_toplevel_resize_event *ev = data;
	struct view *v = wl_container_of(l, v, request_resize);
	if (S.seat->pointer_state.focused_surface == v->xdg_toplevel->base->surface
			&& S.seat->pointer_state.button_count > 0)
		grab_begin(v, GRAB_RESIZE, ev->edges);
}

static void new_xdg_toplevel(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_xdg_toplevel *t = data;
	struct view *v = calloc(1, sizeof *v);
	if (!v) abort();
	v->kind = K_VIEW;
	v->xdg_toplevel = t;
	v->cur_w = v->cur_h = 0;
	v->scene_tree = wlr_scene_xdg_surface_create(S.t_hidden, t->base);
	v->scene_tree->node.data = v;
	t->base->data = v->scene_tree;

	v->map.notify = view_map;
	wl_signal_add(&t->base->surface->events.map, &v->map);
	v->unmap.notify = view_unmap;
	wl_signal_add(&t->base->surface->events.unmap, &v->unmap);
	v->commit.notify = view_commit;
	wl_signal_add(&t->base->surface->events.commit, &v->commit);
	v->destroy.notify = view_destroy;
	wl_signal_add(&t->events.destroy, &v->destroy);
	v->request_maximize.notify = view_request_maximize;
	wl_signal_add(&t->events.request_maximize, &v->request_maximize);
	v->request_fullscreen.notify = view_request_fullscreen;
	wl_signal_add(&t->events.request_fullscreen, &v->request_fullscreen);
	v->request_move.notify = view_request_move;
	wl_signal_add(&t->events.request_move, &v->request_move);
	v->request_resize.notify = view_request_resize;
	wl_signal_add(&t->events.request_resize, &v->request_resize);
}

/* Keep a popup inside the output: clients ask for flip/slide in their positioner
 * and expect the compositor to apply it. The box is in the root surface's coordinates. */
static void popup_unconstrain(struct popup_l *p) {
	if (!S.output || !p->root) return;
	int lx, ly;
	wlr_scene_node_coords(&p->root->node, &lx, &ly);
	struct wlr_box box = { -lx, -ly, S.output->w, S.output->h };
	wlr_xdg_popup_unconstrain_from_box(p->popup, &box);
}

/* Placement must wait for the popup's initial commit: newer wlroots aborts if a
 * configure is scheduled on an xdg_surface that is not initialized yet. */
static void popup_commit(struct wl_listener *l, void *data) {
	(void)data;
	struct popup_l *p = wl_container_of(l, p, commit);
	if (p->popup->base->initial_commit) popup_unconstrain(p);
}

static void popup_reposition(struct wl_listener *l, void *data) {
	(void)data;
	struct popup_l *p = wl_container_of(l, p, reposition);
	popup_unconstrain(p);
}

static void popup_destroy(struct wl_listener *l, void *data) {
	(void)data;
	struct popup_l *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->destroy.link);
	wl_list_remove(&p->reposition.link);
	wl_list_remove(&p->commit.link);
	free(p);
}

static void attach_popup(struct wlr_xdg_popup *popup, struct wlr_scene_tree *parent_tree,
		struct wlr_scene_tree *root) {
	popup->base->data = wlr_scene_xdg_surface_create(parent_tree, popup->base);
	struct popup_l *p = calloc(1, sizeof *p);
	if (!p) abort();
	p->popup = popup;
	p->root = root;
	p->destroy.notify = popup_destroy;
	wl_signal_add(&popup->events.destroy, &p->destroy);
	p->reposition.notify = popup_reposition;
	wl_signal_add(&popup->events.reposition, &p->reposition);
	p->commit.notify = popup_commit;
	wl_signal_add(&popup->base->surface->events.commit, &p->commit);
}

static void new_xdg_popup(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_xdg_popup *popup = data;
	struct wlr_xdg_surface *parent = wlr_xdg_surface_try_from_wlr_surface(popup->parent);
	if (!parent || !parent->data) return;
	/* walk up through nested popups to the toplevel they hang off */
	struct wlr_xdg_surface *top = parent;
	while (top->role == WLR_XDG_SURFACE_ROLE_POPUP && top->popup && top->popup->parent) {
		struct wlr_xdg_surface *up = wlr_xdg_surface_try_from_wlr_surface(top->popup->parent);
		if (!up || !up->data) break;
		top = up;
	}
	attach_popup(popup, parent->data, top->data);
}

/* ------------------------------------------------------------------ */
/* decorations: always server-side == we draw nothing                  */
/* ------------------------------------------------------------------ */

static void deco_request_mode(struct wl_listener *l, void *data) {
	(void)data;
	struct deco *d = wl_container_of(l, d, request_mode);
	if (d->wlr->toplevel->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(d->wlr, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void deco_destroy(struct wl_listener *l, void *data) {
	(void)data;
	struct deco *d = wl_container_of(l, d, destroy);
	wl_list_remove(&d->request_mode.link);
	wl_list_remove(&d->destroy.link);
	free(d);
}

static void new_decoration(struct wl_listener *l, void *data) {
	(void)l;
	struct deco *d = calloc(1, sizeof *d);
	if (!d) abort();
	d->wlr = data;
	d->request_mode.notify = deco_request_mode;
	wl_signal_add(&d->wlr->events.request_mode, &d->request_mode);
	d->destroy.notify = deco_destroy;
	wl_signal_add(&d->wlr->events.destroy, &d->destroy);
	deco_request_mode(&d->request_mode, d->wlr);
}

/* ------------------------------------------------------------------ */
/* layer shell (bars, launchers, wallpaper clients)                    */
/* ------------------------------------------------------------------ */

static void layer_map(struct wl_listener *l, void *data) {
	(void)data;
	struct layer *ly = wl_container_of(l, ly, map);
	ly->mapped = true;
	enum zwlr_layer_shell_v1_layer layer = ly->ls->current.layer;
	if (ly->ls->current.keyboard_interactive == ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
			&& layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP) {
		S.focused_layer = ly;
		keyboard_enter(ly->ls->surface);
	}
	arrange_layers();
}

static void layer_unmap(struct wl_listener *l, void *data) {
	(void)data;
	struct layer *ly = wl_container_of(l, ly, unmap);
	ly->mapped = false;
	if (S.focused_layer == ly) {
		S.focused_layer = NULL;
		if (S.focused && S.focused->focus_view) keyboard_enter(S.focused->focus_view->xdg_toplevel->base->surface);
		else wlr_seat_keyboard_notify_clear_focus(S.seat);
	}
	arrange_layers();
}

static void layer_commit(struct wl_listener *l, void *data) {
	(void)data;
	struct layer *ly = wl_container_of(l, ly, commit);
	if (!ly->ls->initialized) return;
	arrange_layers();
}

static void layer_new_popup(struct wl_listener *l, void *data) {
	struct layer *ly = wl_container_of(l, ly, new_popup);
	attach_popup(data, ly->scene->tree, ly->scene->tree);
}

static void layer_destroy(struct wl_listener *l, void *data) {
	(void)data;
	struct layer *ly = wl_container_of(l, ly, destroy);
	if (S.focused_layer == ly) S.focused_layer = NULL;
	wl_list_remove(&ly->map.link);
	wl_list_remove(&ly->unmap.link);
	wl_list_remove(&ly->commit.link);
	wl_list_remove(&ly->destroy.link);
	wl_list_remove(&ly->new_popup.link);
	wl_list_remove(&ly->link);
	free(ly);
	arrange_layers();
}

static void new_layer_surface(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_layer_surface_v1 *ls = data;
	if (!ls->output) {
		if (!S.output) { wlr_layer_surface_v1_destroy(ls); return; }
		ls->output = S.output->wlr_output;
	}
	struct layer *ly = calloc(1, sizeof *ly);
	if (!ly) abort();
	ly->kind = K_LAYER;
	ly->ls = ls;
	int idx = (int)ls->pending.layer;
	if (idx < 0 || idx > 3) idx = 0;
	ly->scene = wlr_scene_layer_surface_v1_create(S.t_layer[idx], ls);
	ly->scene->tree->node.data = ly;
	ls->data = ly->scene->tree;
	wl_list_insert(&S.layers, &ly->link);

	ly->map.notify = layer_map;
	wl_signal_add(&ls->surface->events.map, &ly->map);
	ly->unmap.notify = layer_unmap;
	wl_signal_add(&ls->surface->events.unmap, &ly->unmap);
	ly->commit.notify = layer_commit;
	wl_signal_add(&ls->surface->events.commit, &ly->commit);
	ly->destroy.notify = layer_destroy;
	wl_signal_add(&ls->events.destroy, &ly->destroy);
	ly->new_popup.notify = layer_new_popup;
	wl_signal_add(&ls->events.new_popup, &ly->new_popup);
}

/* ------------------------------------------------------------------ */
/* actions (keybinds + IPC share these)                                */
/* ------------------------------------------------------------------ */

static void on_swipe_begin(struct wl_listener *l, void *data);
static void on_swipe_update(struct wl_listener *l, void *data);
static void on_swipe_end(struct wl_listener *l, void *data);

static void reply(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void reply(char *buf, size_t n, const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, n, fmt, ap);
	va_end(ap);
}

static void slot_goto_index(int idx, char *out, size_t n) {
	struct slot *s = slot_at(idx);
	if (!s) { reply(out, n, "error: no such slot"); return; }
	focus_slot(s, true);
	reply(out, n, "ok");
}

static void do_reload(char *out, size_t n) {
	if (!cfg_path) cfg_path = config_find_path();
	if (!cfg_path) { reply(out, n, "error: no config file found, keeping current settings"); return; }
	struct config nc;
	char err[300];
	if (!config_load(cfg_path, &nc, err, sizeof err)) {
		reply(out, n, "error: %s (keeping current settings)", err);
		wlr_log(WLR_ERROR, "config reload failed: %s", err);
		return;
	}
	config_free(&cfg);
	cfg = nc;
	log_effective_config();
	relayout();
	reply(out, n, "ok (cursor and keyboard settings need a restart)");
}

#ifdef MONOLITH_TESTING
static void test_cursor_move(void);
static void test_cursor_button(struct wlr_pointer_button_event *ev);
#endif
static void run_action(const char *action, char *out, size_t n) {
	char buf[512];
	snprintf(buf, sizeof buf, "%s", action);
	char *arg = strchr(buf, ' ');
	if (arg) { *arg++ = '\0'; while (*arg == ' ') arg++; }
	const char *cmd = buf;
	int count = slot_count();
	int fidx = S.focused ? slot_index(S.focused) : -1;

	if (!strcmp(cmd, "slot.next") || !strcmp(cmd, "slot.prev")) {
		if (!count) { reply(out, n, "ok"); return; }
		int step = !strcmp(cmd, "slot.next") ? 1 : -1;
		int t = fidx + step;
		if (t < 0 || t >= count) {
			if (!cfg.wrap_around) { reply(out, n, "ok"); return; }
			t = (t + count) % count;
		}
		slot_goto_index(t, out, n);
	} else if (!strcmp(cmd, "slot.goto")) {
		slot_goto_index(arg ? atoi(arg) - 1 : -1, out, n);
	} else if (!strcmp(cmd, "slot.recent")) {
		if (S.last_slot) { focus_slot(S.last_slot, true); reply(out, n, "ok"); }
		else reply(out, n, "ok");
	} else if (!strcmp(cmd, "slot.move-left") || !strcmp(cmd, "slot.move-right")) {
		if (!S.focused) { reply(out, n, "ok"); return; }
		struct slot *o = slot_at(fidx + (!strcmp(cmd, "slot.move-left") ? -1 : 1));
		if (o) { swap_slots(S.focused, o); anim_snap(); relayout(); }
		reply(out, n, "ok");
	} else if (!strcmp(cmd, "slot.swap")) {
		if (!S.focused) { reply(out, n, "ok"); return; }
		int t = arg ? atoi(arg) - 1 : -1;
		if (t < 0) { reply(out, n, "error: bad slot number"); return; }
		struct slot *o = slot_at(t);
		if (o) swap_slots(S.focused, o);
		else { wl_list_remove(&S.focused->link); wl_list_insert(S.slots.prev, &S.focused->link); }
		anim_snap();
		relayout();
		reply(out, n, "ok");
	} else if (!strcmp(cmd, "windows-mode.toggle")) {
		if (!S.focused) { reply(out, n, "ok"); return; }
		slot_set_windows_mode(S.focused, !S.focused->windows_mode);
		S.focused->auto_wm = false;
		play_sound(SND_WINDOWS_MODE);
		relayout();
		reply(out, n, "ok");
	} else if (!strcmp(cmd, "window.close")) {
		if (S.focused && S.focused->focus_view)
			wlr_xdg_toplevel_send_close(S.focused->focus_view->xdg_toplevel);
		reply(out, n, "ok");
	} else if (!strcmp(cmd, "exec")) {
		if (!arg || !*arg) { reply(out, n, "error: exec needs a command"); return; }
		spawn_shell(arg);
		reply(out, n, "ok");
	} else if (!strcmp(cmd, "config.reload") || !strcmp(cmd, "reload")) {
		do_reload(out, n);
	} else if (!strcmp(cmd, "quit")) {
		wl_display_terminate(S.display);
		reply(out, n, "ok");
	} else if (!strcmp(cmd, "query")) {
		size_t off = 0;
		int i = 0;
		struct slot *s;
		out[0] = '\0';
		if (!count) { reply(out, n, "no slots"); return; }
		wl_list_for_each(s, &S.slots, link) {
			const char *app = (s->focus_view && s->focus_view->xdg_toplevel->app_id)
				? s->focus_view->xdg_toplevel->app_id : "?";
			int w = snprintf(out + off, n - off, "%d%s %s%s\n", i + 1,
				s == S.focused ? "*" : " ", app, s->windows_mode ? " [windows]" : "");
			if (w < 0 || (size_t)w >= n - off) break;
			off += (size_t)w;
			i++;
		}
#ifdef MONOLITH_TESTING
	} else if (!strcmp(cmd, "test.pointer")) { /* test.pointer x y: move the cursor like a real pointer */
		double px, py;
		if (!arg || sscanf(arg, "%lf %lf", &px, &py) != 2) { reply(out, n, "error: test.pointer x y"); return; }
		wlr_cursor_warp(S.cursor, NULL, px, py);
		test_cursor_move();
		reply(out, n, "cursor %.0f %.0f", S.cursor->x, S.cursor->y);
	} else if (!strcmp(cmd, "test.click")) { /* test.click [button]: press and release at the cursor */
		int btn = arg ? atoi(arg) : 0x110;
		if (!btn) btn = 0x110;
		struct wlr_pointer_button_event be = { .time_msec = (uint32_t)now_ms(), .button = (uint32_t)btn, .state = WL_POINTER_BUTTON_STATE_PRESSED };
		test_cursor_button(&be);
		be.state = WL_POINTER_BUTTON_STATE_RELEASED;
		be.time_msec += 40;
		test_cursor_button(&be);
		reply(out, n, "clicked");
	} else if (!strcmp(cmd, "test.state")) {
		int act = 0;
		struct slot *as;
		wl_list_for_each(as, &S.slots, link) act += as->img_active ? 1 : 0;
		reply(out, n, "pos=%.3f animating=%d images=%d failed=%d", S.pos, S.animating, act, S.img_failed);
	} else if (!strcmp(cmd, "test.swipe")) { /* test.swipe <dx> [fingers] */
		double dx = arg ? atof(arg) : 0;
		const char *sp = arg ? strchr(arg, ' ') : NULL;
		unsigned fingers = sp ? (unsigned)atoi(sp) : 3;
		struct wlr_pointer_swipe_begin_event b = { .fingers = fingers };
		on_swipe_begin(NULL, &b);
		struct wlr_pointer_swipe_update_event u = { .fingers = fingers, .dx = dx, .dy = 0 };
		on_swipe_update(NULL, &u);
		double mid = S.pos;
		struct wlr_pointer_swipe_end_event e = { .cancelled = false };
		on_swipe_end(NULL, &e);
		reply(out, n, "swiped: pos_during=%.3f animating_after=%d", mid, S.animating);
	} else if (!strcmp(cmd, "test.drag")) { /* test.drag move|resize dx dy (build with -Dtesting=true) */
		char mode[16];
		int dx, dy;
		struct view *v = S.focused ? S.focused->focus_view : NULL;
		if (!arg || sscanf(arg, "%15s %d %d", mode, &dx, &dy) != 3 || !v) {
			reply(out, n, "error: test.drag move|resize dx dy");
			return;
		}
		int slx, sly;
		wlr_scene_node_coords(&S.focused->tree->node, &slx, &sly);
		if (!strcmp(mode, "move")) /* grab near the top-left so there is room to drag right/down */
			wlr_cursor_warp(S.cursor, NULL, slx + v->float_x + 5, sly + v->float_y + 5);
		else /* resize from the bottom-right corner */
			wlr_cursor_warp(S.cursor, NULL, slx + v->float_x + v->float_w - 5, sly + v->float_y + v->float_h - 5);
		if (!grab_begin(v, !strcmp(mode, "move") ? GRAB_MOVE : GRAB_RESIZE, WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM)) {
			reply(out, n, "error: grab refused (not in windows mode?)");
			return;
		}
		wlr_cursor_warp(S.cursor, NULL, S.cursor->x + dx, S.cursor->y + dy);
		grab_motion();
		reply(out, n, "geometry %d %d %d %d", v->float_x, v->float_y, v->float_w, v->float_h);
		grab_end();
#endif
	} else {
		reply(out, n, "error: unknown action \"%s\"", cmd);
	}
}

/* ------------------------------------------------------------------ */
/* IPC socket (monolithctl)                                            */
/* ------------------------------------------------------------------ */

static int ipc_accept(int fd, uint32_t mask, void *data) {
	(void)mask; (void)data;
	int c = accept4(fd, NULL, NULL, SOCK_CLOEXEC);
	if (c < 0) return 0;
	struct timeval tv = { 0, 200000 };
	setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	char in[512];
	ssize_t r = read(c, in, sizeof in - 1);
	if (r > 0) {
		in[r] = '\0';
		while (r > 0 && (in[r - 1] == '\n' || in[r - 1] == '\r')) in[--r] = '\0';
		char out[2048];
		run_action(in, out, sizeof out);
		size_t len = strlen(out);
		if (len == 0 || out[len - 1] != '\n') { out[len++] = '\n'; out[len] = '\0'; }
		ssize_t wr = write(c, out, len);
		(void)wr;
	}
	close(c);
	return 0;
}

static bool ipc_start(const char *wl_socket) {
	const char *rt = getenv("XDG_RUNTIME_DIR");
	if (!rt) return false;
	snprintf(S.ipc_path, sizeof S.ipc_path, "%s/monolith-%s.sock", rt, wl_socket);
	if (strlen(S.ipc_path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) return false;
	S.ipc_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (S.ipc_fd < 0) return false;
	struct sockaddr_un addr = { .sun_family = AF_UNIX };
	memcpy(addr.sun_path, S.ipc_path, strlen(S.ipc_path)); /* length checked above, struct is zeroed */
	unlink(S.ipc_path);
	if (bind(S.ipc_fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(S.ipc_fd, 4) < 0) {
		close(S.ipc_fd);
		S.ipc_fd = -1;
		return false;
	}
	wl_event_loop_add_fd(S.loop, S.ipc_fd, WL_EVENT_READABLE, ipc_accept, NULL);
	setenv("MONOLITH_SOCK", S.ipc_path, 1);
	return true;
}

/* ------------------------------------------------------------------ */
/* keyboard                                                            */
/* ------------------------------------------------------------------ */

static bool handle_keybind(struct wlr_keyboard *kb, uint32_t keycode) {
	const xkb_keysym_t *syms;
	int ns = xkb_state_key_get_syms(kb->xkb_state, keycode, &syms);
	const xkb_keysym_t *base = NULL;
	xkb_layout_index_t layout = xkb_state_key_get_layout(kb->xkb_state, keycode);
	int nb = xkb_keymap_key_get_syms_by_level(kb->keymap, keycode, layout, 0, &base);
	uint32_t mods = wlr_keyboard_get_modifiers(kb) & MODS_MASK;

	for (size_t i = cfg.n_binds; i-- > 0;) { /* later (user) binds win */
		struct keybind *b = &cfg.binds[i];
		if (b->mods != mods) continue;
		bool hit = false;
		for (int j = 0; j < ns && !hit; j++) hit = syms[j] == b->sym;
		for (int j = 0; j < nb && !hit; j++) hit = base[j] == b->sym;
		if (!hit) continue;
		char out[2048];
		run_action(b->action, out, sizeof out);
		if (strncmp(out, "error", 5) == 0)
			wlr_log(strstr(out, "no such slot") ? WLR_DEBUG : WLR_ERROR, "keybind: %s", out);
		return true;
	}
	return false;
}

static void kb_modifiers(struct wl_listener *l, void *data) {
	(void)data;
	struct keyboard *k = wl_container_of(l, k, modifiers);
	wlr_seat_set_keyboard(S.seat, k->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(S.seat, &k->wlr_keyboard->modifiers);
}

static void kb_key(struct wl_listener *l, void *data) {
	struct keyboard *k = wl_container_of(l, k, key);
	struct wlr_keyboard_key_event *ev = data;
	uint32_t keycode = ev->keycode + 8;
	bool handled = false;
	if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED)
		handled = handle_keybind(k->wlr_keyboard, keycode);
	if (!handled) {
		wlr_seat_set_keyboard(S.seat, k->wlr_keyboard);
		wlr_seat_keyboard_notify_key(S.seat, ev->time_msec, ev->keycode, ev->state);
	}
}

static void kb_destroy(struct wl_listener *l, void *data) {
	(void)data;
	struct keyboard *k = wl_container_of(l, k, destroy);
	wl_list_remove(&k->modifiers.link);
	wl_list_remove(&k->key.link);
	wl_list_remove(&k->destroy.link);
	wl_list_remove(&k->link);
	free(k);
}

static void new_keyboard(struct wlr_input_device *dev) {
	struct wlr_keyboard *wk = wlr_keyboard_from_input_device(dev);
	struct keyboard *k = calloc(1, sizeof *k);
	if (!k) abort();
	k->wlr_keyboard = wk;

	struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_rule_names rn = {
		.layout = cfg.kb_layout[0] ? cfg.kb_layout : NULL,
		.variant = cfg.kb_variant[0] ? cfg.kb_variant : NULL,
		.options = cfg.kb_options[0] ? cfg.kb_options : NULL,
	};
	struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &rn, XKB_KEYMAP_COMPILE_NO_FLAGS);
	if (!km) {
		wlr_log(WLR_ERROR, "bad keyboard settings, falling back to default layout");
		km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	}
	wlr_keyboard_set_keymap(wk, km);
	xkb_keymap_unref(km);
	xkb_context_unref(ctx);
	wlr_keyboard_set_repeat_info(wk, cfg.repeat_rate, cfg.repeat_delay);

	k->modifiers.notify = kb_modifiers;
	wl_signal_add(&wk->events.modifiers, &k->modifiers);
	k->key.notify = kb_key;
	wl_signal_add(&wk->events.key, &k->key);
	k->destroy.notify = kb_destroy;
	wl_signal_add(&dev->events.destroy, &k->destroy);

	wlr_seat_set_keyboard(S.seat, wk);
	wl_list_insert(&S.keyboards, &k->link);
}

/* ---- libinput settings (touchpads and mice; other backends have no libinput device) ---- */

static void apply_input_cfg(struct libinput_device *ld, const struct input_cfg *c) {
	bool has_tap = libinput_device_config_tap_get_finger_count(ld) > 0;
	if (has_tap && c->tap >= 0)
		libinput_device_config_tap_set_enabled(ld, c->tap ? LIBINPUT_CONFIG_TAP_ENABLED : LIBINPUT_CONFIG_TAP_DISABLED);
	if (has_tap && c->tap_drag >= 0)
		libinput_device_config_tap_set_drag_enabled(ld, c->tap_drag ? LIBINPUT_CONFIG_DRAG_ENABLED : LIBINPUT_CONFIG_DRAG_DISABLED);
	if (has_tap && c->drag_lock >= 0)
		libinput_device_config_tap_set_drag_lock_enabled(ld, c->drag_lock ? LIBINPUT_CONFIG_DRAG_LOCK_ENABLED : LIBINPUT_CONFIG_DRAG_LOCK_DISABLED);
	if (c->natural_scroll >= 0 && libinput_device_config_scroll_has_natural_scroll(ld))
		libinput_device_config_scroll_set_natural_scroll_enabled(ld, c->natural_scroll);
	if (c->disable_while_typing >= 0 && libinput_device_config_dwt_is_available(ld))
		libinput_device_config_dwt_set_enabled(ld, c->disable_while_typing ? LIBINPUT_CONFIG_DWT_ENABLED : LIBINPUT_CONFIG_DWT_DISABLED);
	if (c->left_handed >= 0 && libinput_device_config_left_handed_is_available(ld))
		libinput_device_config_left_handed_set(ld, c->left_handed);
	if (c->middle_emulation >= 0 && libinput_device_config_middle_emulation_is_available(ld))
		libinput_device_config_middle_emulation_set_enabled(ld, c->middle_emulation ? LIBINPUT_CONFIG_MIDDLE_EMULATION_ENABLED : LIBINPUT_CONFIG_MIDDLE_EMULATION_DISABLED);
	if (libinput_device_config_accel_is_available(ld)) {
		if (c->has_speed) libinput_device_config_accel_set_speed(ld, c->accel_speed);
		if (c->accel_profile >= 0)
			libinput_device_config_accel_set_profile(ld, c->accel_profile ? LIBINPUT_CONFIG_ACCEL_PROFILE_FLAT : LIBINPUT_CONFIG_ACCEL_PROFILE_ADAPTIVE);
	}
	if (c->click_method >= 0) {
		enum libinput_config_click_method m = c->click_method ? LIBINPUT_CONFIG_CLICK_METHOD_BUTTON_AREAS : LIBINPUT_CONFIG_CLICK_METHOD_CLICKFINGER;
		if (libinput_device_config_click_get_methods(ld) & m) libinput_device_config_click_set_method(ld, m);
	}
	if (c->scroll_method >= 0) {
		enum libinput_config_scroll_method m = c->scroll_method == 0 ? LIBINPUT_CONFIG_SCROLL_2FG
			: c->scroll_method == 1 ? LIBINPUT_CONFIG_SCROLL_EDGE : LIBINPUT_CONFIG_SCROLL_ON_BUTTON_DOWN;
		if (libinput_device_config_scroll_get_methods(ld) & m) libinput_device_config_scroll_set_method(ld, m);
	}
}

static void configure_pointer(struct wlr_input_device *dev) {
	if (!wlr_input_device_is_libinput(dev)) return;
	struct libinput_device *ld = wlr_libinput_get_device_handle(dev);
	if (!ld) return;
	bool touchpad = libinput_device_config_tap_get_finger_count(ld) > 0;
	apply_input_cfg(ld, touchpad ? &cfg.touchpad : &cfg.mouse);
	wlr_log(WLR_INFO, "input: %s configured as %s", dev->name ? dev->name : "pointer", touchpad ? "touchpad" : "mouse");
}

/* ---- multi-finger swipe: the strip follows your fingers, then settles on a slot ---- */

static struct { bool active; double start_pos, dx; } swipe;

static void on_swipe_begin(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_pointer_swipe_begin_event *ev = data;
	if (cfg.swipe_fingers <= 0 || (int)ev->fingers != cfg.swipe_fingers || slot_count() < 2 || !S.output || !look_on())
		return;
	swipe.active = true;
	swipe.dx = 0;
	S.animating = false; /* grab the strip wherever it is */
	S.pos = cur_pos();
	S.have_pos = true;
	swipe.start_pos = S.pos;
}

static void on_swipe_update(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_pointer_swipe_update_event *ev = data;
	if (!swipe.active) return;
	swipe.dx += ev->dx;
	double dist = (double)cfg.swipe_distance * S.output->w;
	double p = swipe.start_pos + (cfg.swipe_natural ? -swipe.dx : swipe.dx) / dist;
	int n = slot_count();
	if (p < 0) p = 0;
	if (p > n - 1) p = n - 1;
	S.pos = p;
	relayout();
}

static void on_swipe_end(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_pointer_swipe_end_event *ev = data;
	if (!swipe.active) return;
	swipe.active = false;
	int base = (int)lround(swipe.start_pos), n = slot_count();
	int target = base;
	double moved = S.pos - base;
	if (!ev->cancelled) {
		if (moved > cfg.swipe_commit) target = base + 1;
		else if (moved < -cfg.swipe_commit) target = base - 1;
	}
	if (target < 0) target = 0;
	if (target > n - 1) target = n - 1;
	struct slot *ts = slot_at(target);
	if (ts && ts != S.focused) {
		focus_slot(ts, true);
	} else {
		anim_retarget(base, true); /* not far enough: slide back */
		relayout();
	}
}

static void new_input(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_input_device *dev = data;
	switch (dev->type) {
	case WLR_INPUT_DEVICE_KEYBOARD: new_keyboard(dev); break;
	case WLR_INPUT_DEVICE_POINTER:
		wlr_cursor_attach_input_device(S.cursor, dev);
		configure_pointer(dev);
		break;
	default: break;
	}
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&S.keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(S.seat, caps);
}

/* ------------------------------------------------------------------ */
/* pointer                                                             */
/* ------------------------------------------------------------------ */

/* Returns the owning view/layer (as a pointer to its `kind` member) of the
 * surface under (x,y), or NULL. */
static void *hit_test(double x, double y, struct wlr_surface **surface, double *sx, double *sy) {
	*surface = NULL;
	struct wlr_scene_node *node = wlr_scene_node_at(&S.scene->tree.node, x, y, sx, sy);
	if (!node || node->type != WLR_SCENE_NODE_BUFFER) return NULL;
	struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
	if (!ss) return node->data; /* a neighbour picture (img_tag), or nothing */
	*surface = ss->surface;
	struct wlr_scene_tree *tree = node->parent;
	while (tree && !tree->node.data) tree = tree->node.parent;
	return tree ? tree->node.data : NULL;
}

static void cursor_move(uint32_t time) {
	if (grab.mode != GRAB_NONE) { grab_motion(); return; }
	struct wlr_surface *surface;
	double sx, sy;
	void *owner = hit_test(S.cursor->x, S.cursor->y, &surface, &sx, &sy);
	/* windows in neighbouring slots are look-only: clicking focuses them,
	 * but the pointer does not interact with them. */
	if (owner && *(enum kind *)owner == K_VIEW && ((struct view *)owner)->slot != S.focused)
		surface = NULL;
	if (surface) {
		wlr_seat_pointer_notify_enter(S.seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(S.seat, time, sx, sy);
	} else {
		wlr_cursor_set_xcursor(S.cursor, S.cursor_mgr, "default");
		wlr_seat_pointer_clear_focus(S.seat);
	}
}

static void on_cursor_motion(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_pointer_motion_event *ev = data;
	wlr_cursor_move(S.cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
	cursor_move(ev->time_msec);
}

static void on_cursor_motion_abs(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_pointer_motion_absolute_event *ev = data;
	wlr_cursor_warp_absolute(S.cursor, &ev->pointer->base, ev->x, ev->y);
	cursor_move(ev->time_msec);
}

static void on_cursor_button(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_pointer_button_event *ev = data;
	if (ev->state == WL_POINTER_BUTTON_STATE_RELEASED && grab.mode != GRAB_NONE) {
		grab_end();
		cursor_move(ev->time_msec);
		return;
	}
	if (ev->state == WL_POINTER_BUTTON_STATE_PRESSED) {
		struct wlr_surface *surface;
		double sx, sy;
		void *owner = hit_test(S.cursor->x, S.cursor->y, &surface, &sx, &sy);
		if (owner && *(enum kind *)owner == K_SLOT) { /* click on a neighbour picture */
			struct slot *ns = ((struct img_tag *)owner)->slot;
			if (ns != S.focused) focus_slot(ns, true);
			return;
		}
		if (owner && *(enum kind *)owner == K_VIEW) {
			struct view *v = owner;
			/* Mod+left-drag moves, Mod+right-drag resizes (windows mode only) */
			if (v->slot && v->slot == S.focused && v->slot->windows_mode
					&& current_mods() == cfg.mod_mask
					&& (ev->button == BTN_LEFT || ev->button == BTN_RIGHT)) {
				int slx, sly;
				wlr_scene_node_coords(&v->slot->tree->node, &slx, &sly);
				double mx = (S.cursor->x - slx) - v->float_x - v->float_w / 2.0;
				double my = (S.cursor->y - sly) - v->float_y - v->float_h / 2.0;
				uint32_t edges = (mx < 0 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT)
					| (my < 0 ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
				if (grab_begin(v, ev->button == BTN_LEFT ? GRAB_MOVE : GRAB_RESIZE, edges))
					return;
			}
			if (v->slot && v->slot != S.focused) {
				v->slot->focus_view = v; /* clicking a neighbour focuses it, click not forwarded */
				focus_slot(v->slot, true);
				return;
			}
			/* Only re-focus when focus actually changes. Re-focusing on every click would
			 * steal keyboard focus from an open popup menu and dismiss it before the
			 * click reaches the menu item. */
			if (v->slot && (S.active_view != v || v->slot->focus_view != v)) focus_view(v);
		}
	}
	wlr_seat_pointer_notify_button(S.seat, ev->time_msec, ev->button, ev->state);
}

#ifdef MONOLITH_TESTING
static void test_cursor_move(void) { wlr_seat_set_capabilities(S.seat, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD); cursor_move((uint32_t)now_ms()); wlr_seat_pointer_notify_frame(S.seat); }
static void test_cursor_button(struct wlr_pointer_button_event *ev) { on_cursor_button(NULL, ev); wlr_seat_pointer_notify_frame(S.seat); }
#endif

static void on_cursor_axis(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_pointer_axis_event *ev = data;
	wlr_seat_pointer_notify_axis(S.seat, ev->time_msec, ev->orientation, ev->delta,
		ev->delta_discrete, ev->source, ev->relative_direction);
}

static void on_cursor_frame(struct wl_listener *l, void *data) {
	(void)l; (void)data;
	wlr_seat_pointer_notify_frame(S.seat);
}

static void on_request_cursor(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_seat_pointer_request_set_cursor_event *ev = data;
	if (S.seat->pointer_state.focused_client == ev->seat_client)
		wlr_cursor_set_surface(S.cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
}

static void on_request_shape(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *ev = data;
	if (S.seat->pointer_state.focused_client == ev->seat_client)
		wlr_cursor_set_xcursor(S.cursor, S.cursor_mgr, wlr_cursor_shape_v1_name(ev->shape));
}

static void on_request_set_selection(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_seat_request_set_selection_event *ev = data;
	wlr_seat_set_selection(S.seat, ev->source, ev->serial);
}

static void on_request_set_primary(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_seat_request_set_primary_selection_event *ev = data;
	wlr_seat_set_primary_selection(S.seat, ev->source, ev->serial);
}

/* ------------------------------------------------------------------ */
/* outputs (single output)                                             */
/* ------------------------------------------------------------------ */

static void output_update_size(struct output *o) {
	int w, h;
	wlr_output_effective_resolution(o->wlr_output, &w, &h);
	if (w == o->w && h == o->h) return;
	o->w = w;
	o->h = h;
	if (S.output == o) arrange_layers();
}

static void output_frame(struct wl_listener *l, void *data) {
	(void)data;
	struct output *o = wl_container_of(l, o, frame);
	struct wlr_scene_output *so = wlr_scene_get_scene_output(S.scene, o->wlr_output);
	if (!so) return;
	if (S.animating) anim_step();
	else if (S.img_dirty) { S.img_dirty = false; update_images(); }
	wlr_scene_output_commit(so, NULL);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(so, &now);
	if (S.animating) wlr_output_schedule_frame(o->wlr_output);
}

static void output_request_state(struct wl_listener *l, void *data) {
	struct output *o = wl_container_of(l, o, request_state);
	const struct wlr_output_event_request_state *ev = data;
	wlr_output_commit_state(o->wlr_output, ev->state);
	output_update_size(o);
}

static void output_commit(struct wl_listener *l, void *data) {
	(void)data;
	struct output *o = wl_container_of(l, o, commit);
	output_update_size(o);
}

static void output_destroy(struct wl_listener *l, void *data) {
	(void)data;
	struct output *o = wl_container_of(l, o, destroy);
	wl_list_remove(&o->frame.link);
	wl_list_remove(&o->request_state.link);
	wl_list_remove(&o->commit.link);
	wl_list_remove(&o->destroy.link);
	if (S.output == o) S.output = NULL;
	free(o);
}

static void new_output(struct wl_listener *l, void *data) {
	(void)l;
	struct wlr_output *wo = data;
	wlr_output_init_render(wo, S.allocator, S.renderer);

	struct wlr_output_state st;
	wlr_output_state_init(&st);
	wlr_output_state_set_enabled(&st, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wo);
	if (mode) wlr_output_state_set_mode(&st, mode);
	wlr_output_commit_state(wo, &st);
	wlr_output_state_finish(&st);

	struct output *o = calloc(1, sizeof *o);
	if (!o) abort();
	o->wlr_output = wo;
	o->frame.notify = output_frame;
	wl_signal_add(&wo->events.frame, &o->frame);
	o->request_state.notify = output_request_state;
	wl_signal_add(&wo->events.request_state, &o->request_state);
	o->commit.notify = output_commit;
	wl_signal_add(&wo->events.commit, &o->commit);
	o->destroy.notify = output_destroy;
	wl_signal_add(&wo->events.destroy, &o->destroy);

	struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(S.output_layout, wo);
	struct wlr_scene_output *so = wlr_scene_output_create(S.scene, wo);
	wlr_scene_output_layout_add_output(S.scene_layout, lo, so);

	if (!S.output) { /* only the first output carries the strip */
		S.output = o;
		wlr_output_effective_resolution(wo, &o->w, &o->h);
		arrange_layers();
		wlr_log(WLR_INFO, "strip output: %s (%dx%d)", wo->name, o->w, o->h);
	} else {
		wlr_log(WLR_INFO, "extra output %s ignored (multi-monitor is not supported yet)", wo->name);
	}
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

static int term_handler(int sig, void *data) {
	(void)sig; (void)data;
	wl_display_terminate(S.display);
	return 0;
}

static void usage(const char *prog) {
	printf("usage: %s [-c config.toml] [-s startup-command] [-v] [-h]\n", prog);
}

int main(int argc, char **argv) {
	const char *startup = NULL;
	int opt_debug = getenv("MONOLITH_DEBUG") != NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-c") && i + 1 < argc) cfg_path = strdup(argv[++i]);
		else if (!strcmp(argv[i], "-s") && i + 1 < argc) startup = argv[++i];
		else if (!strcmp(argv[i], "-v")) opt_debug = 1;
		else { usage(argv[0]); return !strcmp(argv[i], "-h") ? 0 : 1; }
	}
	wlr_log_init(opt_debug ? WLR_DEBUG : WLR_INFO, NULL);

	config_defaults(&cfg);
	if (!cfg_path) cfg_path = config_find_path();
	if (cfg_path) {
		struct config nc;
		char err[300];
		if (config_load(cfg_path, &nc, err, sizeof err)) {
			config_free(&cfg);
			cfg = nc;
			wlr_log(WLR_INFO, "config: %s", cfg_path);
			log_effective_config();
		} else {
			wlr_log(WLR_ERROR, "config error, using defaults: %s", err);
		}
	} else {
		wlr_log(WLR_INFO, "no config file found, using defaults");
	}

	/* running inside another Wayland/X11 session? (checked before we set our own WAYLAND_DISPLAY) */
	bool nested_host = (getenv("WAYLAND_DISPLAY") && *getenv("WAYLAND_DISPLAY")) ||
		(getenv("DISPLAY") && *getenv("DISPLAY"));

	S.display = wl_display_create();
	S.loop = wl_display_get_event_loop(S.display);
	S.ipc_fd = -1;
	wl_list_init(&S.keyboards);
	wl_list_init(&S.slots);
	wl_list_init(&S.layers);
	wl_event_loop_add_signal(S.loop, SIGCHLD, sigchld_handler, NULL);
	wl_event_loop_add_signal(S.loop, SIGTERM, term_handler, NULL);
	wl_event_loop_add_signal(S.loop, SIGINT, term_handler, NULL);

	S.backend = wlr_backend_autocreate(S.loop, NULL);
	if (!S.backend) { wlr_log(WLR_ERROR, "failed to create backend"); return 1; }
	S.renderer = wlr_renderer_autocreate(S.backend);
	if (!S.renderer) { wlr_log(WLR_ERROR, "failed to create renderer"); return 1; }
	wlr_renderer_init_wl_display(S.renderer, S.display);
	S.allocator = wlr_allocator_autocreate(S.backend, S.renderer);
	if (!S.allocator) { wlr_log(WLR_ERROR, "failed to create allocator"); return 1; }

	wlr_compositor_create(S.display, 5, S.renderer);
	wlr_subcompositor_create(S.display);
	wlr_data_device_manager_create(S.display);
	wlr_primary_selection_v1_device_manager_create(S.display);
	wlr_data_control_manager_v1_create(S.display);
	wlr_viewporter_create(S.display);
	wlr_single_pixel_buffer_manager_v1_create(S.display);
	wlr_screencopy_manager_v1_create(S.display);

	S.output_layout = wlr_output_layout_create(S.display);
	wlr_xdg_output_manager_v1_create(S.display, S.output_layout);

	S.scene = wlr_scene_create();
	S.scene_layout = wlr_scene_attach_output_layout(S.scene, S.output_layout);

	/* stacking order, bottom to top */
	float black[4] = { 0, 0, 0, 1 };
	S.bg = wlr_scene_rect_create(&S.scene->tree, 0, 0, black);
	S.t_layer[ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND] = wlr_scene_tree_create(&S.scene->tree);
	S.t_layer[ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM] = wlr_scene_tree_create(&S.scene->tree);
	S.t_strip = wlr_scene_tree_create(&S.scene->tree);
	S.t_side = wlr_scene_tree_create(&S.scene->tree);   /* tilted neighbour pictures */
	S.t_float = wlr_scene_tree_create(&S.scene->tree);   /* windows-mode windows, above all main apps */
	S.t_layer[ZWLR_LAYER_SHELL_V1_LAYER_TOP] = wlr_scene_tree_create(&S.scene->tree);
	S.t_layer[ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY] = wlr_scene_tree_create(&S.scene->tree);
	S.t_hidden = wlr_scene_tree_create(&S.scene->tree);
	wlr_scene_node_set_enabled(&S.t_hidden->node, false);

	S.new_output.notify = new_output;
	wl_signal_add(&S.backend->events.new_output, &S.new_output);

	S.xdg_shell = wlr_xdg_shell_create(S.display, 3);
	S.new_xdg_toplevel.notify = new_xdg_toplevel;
	wl_signal_add(&S.xdg_shell->events.new_toplevel, &S.new_xdg_toplevel);
	S.new_xdg_popup.notify = new_xdg_popup;
	wl_signal_add(&S.xdg_shell->events.new_popup, &S.new_xdg_popup);

	S.layer_shell = wlr_layer_shell_v1_create(S.display, 4);
	S.new_layer_surface.notify = new_layer_surface;
	wl_signal_add(&S.layer_shell->events.new_surface, &S.new_layer_surface);

	struct wlr_xdg_foreign_registry *foreign = wlr_xdg_foreign_registry_create(S.display);
	wlr_xdg_foreign_v1_create(S.display, foreign);
	wlr_xdg_foreign_v2_create(S.display, foreign);

	S.deco_mgr = wlr_xdg_decoration_manager_v1_create(S.display);
	S.new_decoration.notify = new_decoration;
	wl_signal_add(&S.deco_mgr->events.new_toplevel_decoration, &S.new_decoration);

	S.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(S.cursor, S.output_layout);
	S.cursor_mgr = wlr_xcursor_manager_create(cfg.cursor_theme[0] ? cfg.cursor_theme : NULL,
		(uint32_t)cfg.cursor_size);
	S.cursor_motion.notify = on_cursor_motion;
	wl_signal_add(&S.cursor->events.motion, &S.cursor_motion);
	S.cursor_motion_abs.notify = on_cursor_motion_abs;
	wl_signal_add(&S.cursor->events.motion_absolute, &S.cursor_motion_abs);
	S.cursor_button.notify = on_cursor_button;
	wl_signal_add(&S.cursor->events.button, &S.cursor_button);
	S.cursor_axis.notify = on_cursor_axis;
	wl_signal_add(&S.cursor->events.axis, &S.cursor_axis);
	S.cursor_frame.notify = on_cursor_frame;
	wl_signal_add(&S.cursor->events.frame, &S.cursor_frame);

	S.swipe_begin.notify = on_swipe_begin;
	wl_signal_add(&S.cursor->events.swipe_begin, &S.swipe_begin);
	S.swipe_update.notify = on_swipe_update;
	wl_signal_add(&S.cursor->events.swipe_update, &S.swipe_update);
	S.swipe_end.notify = on_swipe_end;
	wl_signal_add(&S.cursor->events.swipe_end, &S.swipe_end);

	S.cursor_shape = wlr_cursor_shape_manager_v1_create(S.display, 1);
	S.request_shape.notify = on_request_shape;
	wl_signal_add(&S.cursor_shape->events.request_set_shape, &S.request_shape);

	S.seat = wlr_seat_create(S.display, "seat0");
	S.new_input.notify = new_input;
	wl_signal_add(&S.backend->events.new_input, &S.new_input);
	S.request_cursor.notify = on_request_cursor;
	wl_signal_add(&S.seat->events.request_set_cursor, &S.request_cursor);
	S.request_set_selection.notify = on_request_set_selection;
	wl_signal_add(&S.seat->events.request_set_selection, &S.request_set_selection);
	S.request_set_primary.notify = on_request_set_primary;
	wl_signal_add(&S.seat->events.request_set_primary_selection, &S.request_set_primary);

	S.img_timer = wl_event_loop_add_timer(S.loop, img_timer_cb, NULL);
	wl_event_source_timer_update(S.img_timer, 100);

	const char *sock = wl_display_add_socket_auto(S.display);
	if (!sock) {
		wlr_log(WLR_ERROR, "failed to open wayland socket");
		wlr_backend_destroy(S.backend);
		return 1;
	}
	if (!wlr_backend_start(S.backend)) {
		wlr_log(WLR_ERROR, "failed to start backend");
		wlr_backend_destroy(S.backend);
		wl_display_destroy(S.display);
		return 1;
	}

	setenv("WAYLAND_DISPLAY", sock, 1);
	setenv("XDG_SESSION_TYPE", "wayland", 1);
	setenv("XDG_CURRENT_DESKTOP", "monolith", 1);
	/* Portals (file dialogs, screen capture) are started by D-Bus activation and read
	 * WAYLAND_DISPLAY / XDG_CURRENT_DESKTOP from the user environment, like every
	 * compositor session does. When we run nested inside another desktop that would
	 * hijack the host's environment, so "auto" skips it in that case. */
	if (cfg.dbus_env == 1 || (cfg.dbus_env == 0 && !nested_host)) {
		char *dbus_argv[] = { "dbus-update-activation-environment", "--systemd",
			"WAYLAND_DISPLAY", "XDG_CURRENT_DESKTOP", "XDG_SESSION_TYPE", "MONOLITH_SOCK", NULL };
		spawn_argv(dbus_argv, true);
	}
	if (ipc_start(sock)) wlr_log(WLR_INFO, "ipc socket: %s", S.ipc_path);
	else wlr_log(WLR_ERROR, "ipc socket unavailable (monolithctl will not work)");

	for (size_t i = 0; i < cfg.n_autostart; i++) spawn_argv(cfg.autostart[i], false);
	if (startup) spawn_shell(startup);

	wlr_log(WLR_INFO, "MONOLITH running on WAYLAND_DISPLAY=%s", sock);
	wl_display_run(S.display);

	if (S.img_timer) wl_event_source_remove(S.img_timer);
	if (S.ipc_fd >= 0) { close(S.ipc_fd); unlink(S.ipc_path); }
	/* wlroots >= 0.19 asserts that no listener is left on an object being
	 * destroyed, so detach every server-level listener first. */
	wl_list_remove(&S.new_output.link);
	wl_list_remove(&S.new_xdg_toplevel.link);
	wl_list_remove(&S.new_xdg_popup.link);
	wl_list_remove(&S.new_layer_surface.link);
	wl_list_remove(&S.new_decoration.link);
	wl_list_remove(&S.new_input.link);
	wl_list_remove(&S.request_cursor.link);
	wl_list_remove(&S.request_set_selection.link);
	wl_list_remove(&S.request_set_primary.link);
	wl_list_remove(&S.cursor_motion.link);
	wl_list_remove(&S.cursor_motion_abs.link);
	wl_list_remove(&S.cursor_button.link);
	wl_list_remove(&S.cursor_axis.link);
	wl_list_remove(&S.cursor_frame.link);
	wl_list_remove(&S.request_shape.link);
	wl_list_remove(&S.swipe_begin.link);
	wl_list_remove(&S.swipe_update.link);
	wl_list_remove(&S.swipe_end.link);
	wl_display_destroy_clients(S.display);
	wlr_scene_node_destroy(&S.scene->tree.node);
	wlr_xcursor_manager_destroy(S.cursor_mgr);
	wlr_cursor_destroy(S.cursor);
	wlr_allocator_destroy(S.allocator);
	wlr_renderer_destroy(S.renderer);
	wlr_backend_destroy(S.backend);
	wl_display_destroy(S.display);
	config_free(&cfg);
	return 0;
}
