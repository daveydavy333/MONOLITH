#ifndef MONOLITH_CONFIG_H
#define MONOLITH_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <xkbcommon/xkbcommon.h>

enum snd_event {
	SND_SLOT_SWITCH,
	SND_SLOT_OPEN,
	SND_SLOT_CLOSE,
	SND_WINDOWS_MODE,
	SND_COUNT,
};

enum snd_overlap { OVERLAP_ALLOW, OVERLAP_RESTART, OVERLAP_DROP };
enum close_focus { CLOSE_LEFT, CLOSE_RIGHT, CLOSE_RECENT };
/* all curves start at full speed and slow down towards the end ("instant start, ease out") */
enum easing { EASE_SINE_OUT, EASE_CUBIC_OUT, EASE_QUAD_OUT, EASE_LINEAR,
	EASE_EXPO_OUT, EASE_QUART_OUT, EASE_QUINT_OUT, EASE_BEZIER };

/* libinput device settings; -1 = leave the device default */
struct input_cfg {
	int tap, tap_drag, drag_lock;
	int natural_scroll, disable_while_typing, left_handed, middle_emulation;
	int accel_profile;          /* 0 adaptive, 1 flat */
	int click_method;           /* 0 clickfinger, 1 button_areas */
	int scroll_method;          /* 0 two_finger, 1 edge, 2 on_button_down */
	bool has_speed;
	double accel_speed;         /* -1 .. 1 */
};

struct keybind {
	uint32_t mods;      /* WLR_MODIFIER_* mask */
	xkb_keysym_t sym;
	char *action;
};

struct rule {
	char *app_id;       /* fnmatch() pattern */
	bool windows_mode;  /* open this app's slot in windows mode */
	bool new_slot;      /* never join another slot, always open a new one */
};

struct config {
	/* [general] */
	uint32_t mod_mask;          /* what "Mod" means in keybinds */
	bool reduced_motion;        /* master switch: no animation */
	int dbus_env;               /* push WAYLAND_DISPLAY etc. into the D-Bus/systemd user
	                             * environment: 0 auto (only when not nested), 1 always, 2 never */

	/* [strip] */
	float focus_width;          /* fraction of output width */
	float peek;                 /* visible width of each neighbour */
	int gap;
	int edge_gap;               /* added to all four margins */
	int edge_gap_top, edge_gap_bottom; /* override edge_gap for top/bottom, -1 = use edge_gap */
	int margin_top, margin_bottom, margin_left, margin_right;
	bool new_slot_at_right;     /* new slots open at the right (true) or left end */
	bool wrap_around;
	enum close_focus close_focus;

	/* [animation] slot switching */
	bool anim_enabled;
	int anim_ms;
	enum easing anim_easing;
	float anim_bez[4];          /* cubic-bezier control points when anim_easing == EASE_BEZIER */
	float anim_jump;            /* fraction of the move taken instantly at the very start */

	/* [look] how the neighbouring slots are drawn */
	float look_tilt;            /* degrees; > 0 turns the outer edge toward you, < 0 the other way */
	float look_scale;           /* size of a neighbour relative to the focused slot */
	float look_dim;             /* 0 = unchanged, 1 = black */
	float look_persp;           /* camera distance in output widths (smaller = stronger perspective) */

	/* [input.touchpad] / [input.mouse] */
	struct input_cfg touchpad, mouse;

	/* [gestures] */
	int swipe_fingers;          /* 0 = off */
	float swipe_distance;       /* fraction of the output width that moves the strip by one slot */
	float swipe_commit;         /* fraction of a slot you must drag to commit to the next slot */
	bool swipe_natural;         /* strip follows the fingers */

	/* [windows_mode] */
	bool popups_auto;           /* windows with a parent join the parent's slot */
	bool same_client_join;      /* other windows of an app already in a slot join it */
	bool revert_when_single;
	float default_w, default_h; /* fraction of slot size */
	int wm_margin;

	/* [sound] */
	bool sound_enabled;
	int sound_cooldown_ms;
	enum snd_overlap sound_overlap;
	char **sound_cmd[SND_COUNT]; /* NULL-terminated argv, or NULL = silent */

	/* [background] */
	float bg_color[4];

	/* [cursor] */
	char *cursor_theme;
	int cursor_size;

	/* [keyboard] */
	char *kb_layout, *kb_variant, *kb_options;
	int repeat_rate, repeat_delay;

	/* [[rules]], [keybinds], [[autostart]] */
	struct rule *rules;
	size_t n_rules;
	struct keybind *binds;
	size_t n_binds;
	char ***autostart;          /* array of argv arrays */
	size_t n_autostart;
};

/* Fill cfg with built-in defaults (including the default keybinds). */
void config_defaults(struct config *cfg);

/* Parse path on top of the defaults. On failure returns false, writes a
 * message to err and leaves *out untouched. */
bool config_load(const char *path, struct config *out, char *err, size_t errlen);

void config_free(struct config *cfg);

/* Default config path: $XDG_CONFIG_HOME/monolith/config.toml, falling back
 * to ~/.config/monolith/config.toml, then /etc/monolith/config.toml.
 * Returns a malloc'd string or NULL if none exists. */
char *config_find_path(void);

#endif
