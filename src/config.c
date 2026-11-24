#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <wlr/types/wlr_keyboard.h>

#include "config.h"
#include "tomlc17.h"

#define MODS_MASK (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)

/* ---------- small helpers ---------- */

static char *xstrdup(const char *s) {
	char *r = strdup(s ? s : "");
	if (!r) abort();
	return r;
}

static void free_strv(char **v) {
	if (!v) return;
	for (char **p = v; *p; p++) free(*p);
	free(v);
}

static bool get_bool(toml_datum_t t, const char *key, bool *out) {
	toml_datum_t d = toml_seek(t, key);
	if (d.type != TOML_BOOLEAN) return false;
	*out = d.u.boolean;
	return true;
}

static bool get_num(toml_datum_t t, const char *key, double *out) {
	toml_datum_t d = toml_seek(t, key);
	if (d.type == TOML_FP64) { *out = d.u.fp64; return true; }
	if (d.type == TOML_INT64) { *out = (double)d.u.int64; return true; }
	return false;
}

static bool get_int(toml_datum_t t, const char *key, int *out) {
	double v;
	if (!get_num(t, key, &v)) return false;
	*out = (int)v;
	return true;
}

static bool get_float(toml_datum_t t, const char *key, float *out) {
	double v;
	if (!get_num(t, key, &v)) return false;
	*out = (float)v;
	return true;
}

static const char *get_str(toml_datum_t t, const char *key) {
	toml_datum_t d = toml_seek(t, key);
	return d.type == TOML_STRING ? d.u.s : NULL;
}

static void set_str(char **dst, const char *v) {
	if (!v) return;
	free(*dst);
	*dst = xstrdup(v);
}

/* array of strings -> NULL-terminated argv (empty array -> NULL, i.e. "off") */
static char **strv_from(toml_datum_t arr) {
	if (arr.type != TOML_ARRAY || arr.u.arr.size == 0) return NULL;
	char **v = calloc((size_t)arr.u.arr.size + 1, sizeof *v);
	if (!v) abort();
	int n = 0;
	for (int i = 0; i < arr.u.arr.size; i++) {
		toml_datum_t e = arr.u.arr.elem[i];
		if (e.type == TOML_STRING) v[n++] = xstrdup(e.u.s);
	}
	if (n == 0) { free(v); return NULL; }
	return v;
}

/* Sound commands run without a shell, so expand a leading "~/" and $VAR / ${VAR} ourselves:
 * a path that works when typed in a terminal ("~/sounds/x.wav") would otherwise not exist. */
static char *expand_arg(const char *in) {
	size_t cap = strlen(in) * 2 + 4096, len = 0;
	char *out = malloc(cap);
	if (!out) abort();
	const char *home = getenv("HOME");
	if (in[0] == '~' && (in[1] == '/' || in[1] == 0) && home) {
		len = (size_t)snprintf(out, cap, "%s", home);
		in++;
	}
	while (*in && len + 1 < cap) {
		if (*in == '$' && (isalpha((unsigned char)in[1]) || in[1] == '_' || in[1] == '{')) {
			bool br = in[1] == '{';
			const char *a = in + 1 + br, *e = a;
			while (isalnum((unsigned char)*e) || *e == '_') e++;
			char name[128];
			size_t nl = (size_t)(e - a);
			if (nl > 0 && nl < sizeof name && (!br || *e == '}')) {
				memcpy(name, a, nl); name[nl] = 0;
				const char *val = getenv(name);
				if (val) len += (size_t)snprintf(out + len, cap - len, "%s", val);
				in = e + (br ? 1 : 0);
				continue;
			}
		}
		out[len++] = *in++;
	}
	out[len] = 0;
	return out;
}

static char **sound_cmd_from(toml_datum_t d, const char *key) {
	if (d.type == TOML_STRING) { /* "aplay -q x.wav": run through sh -c, like a shell line */
		char **v = calloc(4, sizeof *v);
		if (!v) abort();
		v[0] = xstrdup("sh"); v[1] = xstrdup("-c"); v[2] = xstrdup(d.u.s);
		return v;
	}
	if (d.type != TOML_ARRAY) {
		fprintf(stderr, "monolith: config: %s must be an array of strings (or one string), ignored\n", key);
		return NULL;
	}
	char **v = strv_from(d);
	for (int i = 0; v && v[i]; i++) {
		char *e = expand_arg(v[i]);
		free(v[i]);
		v[i] = e;
	}
	return v;
}

/* "#rrggbb" or "#rrggbbaa" */
static bool parse_color(const char *s, float out[4]) {
	if (!s || s[0] != '#') return false;
	size_t n = strlen(s);
	if (n != 7 && n != 9) return false;
	unsigned v[4] = {0, 0, 0, 255};
	for (size_t i = 0; i < (n - 1) / 2; i++) {
		char b[3] = { s[1 + i * 2], s[2 + i * 2], 0 };
		char *end;
		v[i] = (unsigned)strtoul(b, &end, 16);
		if (*end) return false;
	}
	for (int i = 0; i < 4; i++) out[i] = (float)v[i] / 255.0f;
	return true;
}

/* ---------- keybinds ---------- */

static bool parse_bind(const struct config *cfg, const char *spec,
		uint32_t *mods, xkb_keysym_t *sym) {
	char buf[128];
	if (strlen(spec) >= sizeof buf) return false;
	strcpy(buf, spec);
	*mods = 0;
	*sym = XKB_KEY_NoSymbol;

	char *save = NULL;
	char *tok = strtok_r(buf, "+", &save);
	while (tok) {
		char *next = strtok_r(NULL, "+", &save);
		if (next) { /* modifier token */
			if (!strcasecmp(tok, "Mod")) *mods |= cfg->mod_mask;
			else if (!strcasecmp(tok, "Alt")) *mods |= WLR_MODIFIER_ALT;
			else if (!strcasecmp(tok, "Super") || !strcasecmp(tok, "Logo")) *mods |= WLR_MODIFIER_LOGO;
			else if (!strcasecmp(tok, "Ctrl") || !strcasecmp(tok, "Control")) *mods |= WLR_MODIFIER_CTRL;
			else if (!strcasecmp(tok, "Shift")) *mods |= WLR_MODIFIER_SHIFT;
			else return false;
		} else { /* key token */
			*sym = xkb_keysym_from_name(tok, XKB_KEYSYM_CASE_INSENSITIVE);
		}
		tok = next;
	}
	return *sym != XKB_KEY_NoSymbol;
}

static void add_bind(struct config *cfg, const char *spec, const char *action) {
	uint32_t mods;
	xkb_keysym_t sym;
	if (!parse_bind(cfg, spec, &mods, &sym)) {
		fprintf(stderr, "monolith: config: bad keybind \"%s\" ignored\n", spec);
		return;
	}
	struct keybind *nb = realloc(cfg->binds, (cfg->n_binds + 1) * sizeof *nb);
	if (!nb) abort();
	cfg->binds = nb;
	cfg->binds[cfg->n_binds++] = (struct keybind){ mods & MODS_MASK, sym, xstrdup(action) };
}

/* ---------- defaults ---------- */

static void input_defaults(struct input_cfg *c) {
	c->tap = c->tap_drag = c->drag_lock = -1;
	c->natural_scroll = c->disable_while_typing = c->left_handed = c->middle_emulation = -1;
	c->accel_profile = c->click_method = c->scroll_method = -1;
	c->has_speed = false;
	c->accel_speed = 0;
}

static void parse_input(toml_datum_t root, const char *sect, struct input_cfg *c) {
	char key[96];
	bool b;
	const char *s;
	double v;
#define BOOLKEY(name, field) \
	snprintf(key, sizeof key, "%s." name, sect); \
	if (get_bool(root, key, &b)) c->field = b ? 1 : 0;
	BOOLKEY("tap", tap)
	BOOLKEY("tap_and_drag", tap_drag)
	BOOLKEY("drag_lock", drag_lock)
	BOOLKEY("natural_scroll", natural_scroll)
	BOOLKEY("disable_while_typing", disable_while_typing)
	BOOLKEY("left_handed", left_handed)
	BOOLKEY("middle_emulation", middle_emulation)
#undef BOOLKEY
	snprintf(key, sizeof key, "%s.accel_speed", sect);
	if (get_num(root, key, &v)) { c->has_speed = true; c->accel_speed = v < -1 ? -1 : v > 1 ? 1 : v; }
	snprintf(key, sizeof key, "%s.accel_profile", sect);
	if ((s = get_str(root, key))) c->accel_profile = !strcasecmp(s, "flat") ? 1 : !strcasecmp(s, "adaptive") ? 0 : -1;
	snprintf(key, sizeof key, "%s.click_method", sect);
	if ((s = get_str(root, key))) c->click_method = !strcasecmp(s, "clickfinger") ? 0 : !strcasecmp(s, "button_areas") ? 1 : -1;
	snprintf(key, sizeof key, "%s.scroll_method", sect);
	if ((s = get_str(root, key)))
		c->scroll_method = !strcasecmp(s, "two_finger") ? 0 : !strcasecmp(s, "edge") ? 1
			: !strcasecmp(s, "on_button_down") ? 2 : -1;
}

void config_defaults(struct config *cfg) {
	memset(cfg, 0, sizeof *cfg);
	cfg->mod_mask = WLR_MODIFIER_ALT; /* Alt so it works nested; use Super on real hardware */
	cfg->focus_width = 0.72f;
	cfg->peek = 0.09f;
	cfg->gap = 24;
	cfg->edge_gap_top = cfg->edge_gap_bottom = -1;
	cfg->new_slot_at_right = true;
	cfg->close_focus = CLOSE_LEFT;
	cfg->anim_enabled = true;
	cfg->anim_ms = 200;
	cfg->anim_easing = EASE_EXPO_OUT;
	cfg->anim_bez[0] = 0.05f; cfg->anim_bez[1] = 0.8f; cfg->anim_bez[2] = 0.15f; cfg->anim_bez[3] = 1.0f;
	cfg->anim_jump = 0.0f;
	cfg->swipe_fingers = 3;
	cfg->swipe_distance = 0.6f;
	cfg->swipe_commit = 0.2f;
	cfg->swipe_natural = true;
	input_defaults(&cfg->touchpad);
	input_defaults(&cfg->mouse);
	cfg->touchpad.tap = 1;
	cfg->touchpad.disable_while_typing = 1;
	cfg->look_tilt = 45.0f;
	cfg->look_scale = 0.8f;
	cfg->look_dim = 0.45f;
	cfg->look_persp = 1.6f;
	cfg->popups_auto = true;
	cfg->same_client_join = true;
	cfg->revert_when_single = true;
	cfg->default_w = cfg->default_h = 0.6f;
	cfg->wm_margin = 16;
	cfg->sound_enabled = true;
	cfg->sound_cooldown_ms = 60;
	cfg->sound_overlap = OVERLAP_RESTART;
	cfg->bg_color[0] = cfg->bg_color[1] = cfg->bg_color[2] = 0.0f;
	cfg->bg_color[3] = 1.0f;
	cfg->cursor_theme = xstrdup("");
	cfg->cursor_size = 24;
	cfg->kb_layout = xstrdup("");
	cfg->kb_variant = xstrdup("");
	cfg->kb_options = xstrdup("");
	cfg->repeat_rate = 30;
	cfg->repeat_delay = 300;

	add_bind(cfg, "Mod+Right", "slot.next");
	add_bind(cfg, "Mod+Left", "slot.prev");
	add_bind(cfg, "Mod+Tab", "slot.recent");
	add_bind(cfg, "Mod+Shift+Right", "slot.move-right");
	add_bind(cfg, "Mod+Shift+Left", "slot.move-left");
	add_bind(cfg, "Mod+W", "windows-mode.toggle");
	add_bind(cfg, "Mod+Shift+Q", "window.close");
	add_bind(cfg, "Mod+Shift+R", "config.reload");
	add_bind(cfg, "Mod+Shift+E", "quit");
	for (int i = 1; i <= 9; i++) {
		char spec[32], act[32];
		snprintf(spec, sizeof spec, "Mod+%d", i);
		snprintf(act, sizeof act, "slot.goto %d", i);
		add_bind(cfg, spec, act);
		snprintf(spec, sizeof spec, "Mod+Shift+%d", i);
		snprintf(act, sizeof act, "slot.swap %d", i);
		add_bind(cfg, spec, act);
	}
}

void config_free(struct config *cfg) {
	for (int i = 0; i < SND_COUNT; i++) free_strv(cfg->sound_cmd[i]);
	free(cfg->cursor_theme);
	free(cfg->kb_layout);
	free(cfg->kb_variant);
	free(cfg->kb_options);
	for (size_t i = 0; i < cfg->n_rules; i++) free(cfg->rules[i].app_id);
	free(cfg->rules);
	for (size_t i = 0; i < cfg->n_binds; i++) free(cfg->binds[i].action);
	free(cfg->binds);
	for (size_t i = 0; i < cfg->n_autostart; i++) free_strv(cfg->autostart[i]);
	free(cfg->autostart);
	memset(cfg, 0, sizeof *cfg);
}

/* ---------- loading ---------- */

static float clampf(float v, float lo, float hi) {
	return v < lo ? lo : v > hi ? hi : v;
}

static void apply(struct config *cfg, toml_datum_t root) {
	const char *s;

	if ((s = get_str(root, "general.mod"))) {
		uint32_t m = 0;
		if (!strcasecmp(s, "Alt")) m = WLR_MODIFIER_ALT;
		else if (!strcasecmp(s, "Super") || !strcasecmp(s, "Logo")) m = WLR_MODIFIER_LOGO;
		else if (!strcasecmp(s, "Ctrl") || !strcasecmp(s, "Control")) m = WLR_MODIFIER_CTRL;
		else fprintf(stderr, "monolith: config: unknown general.mod \"%s\"\n", s);
		if (m) cfg->mod_mask = m;
	}
	get_bool(root, "general.reduced_motion", &cfg->reduced_motion);
	if ((s = get_str(root, "general.dbus_env"))) {
		if (!strcasecmp(s, "always")) cfg->dbus_env = 1;
		else if (!strcasecmp(s, "never")) cfg->dbus_env = 2;
		else cfg->dbus_env = 0;
	}

	get_float(root, "strip.focus_width", &cfg->focus_width);
	get_float(root, "strip.peek", &cfg->peek);
	get_int(root, "strip.gap", &cfg->gap);
	get_int(root, "strip.edge_gap", &cfg->edge_gap);
	if (cfg->edge_gap < 0) cfg->edge_gap = 0;
	get_int(root, "strip.edge_gap_top", &cfg->edge_gap_top);
	get_int(root, "strip.edge_gap_bottom", &cfg->edge_gap_bottom);
	if (cfg->edge_gap_top < -1) cfg->edge_gap_top = -1;
	if (cfg->edge_gap_bottom < -1) cfg->edge_gap_bottom = -1;
	get_int(root, "strip.margin.top", &cfg->margin_top);
	get_int(root, "strip.margin.bottom", &cfg->margin_bottom);
	get_int(root, "strip.margin.left", &cfg->margin_left);
	get_int(root, "strip.margin.right", &cfg->margin_right);
	if ((s = get_str(root, "strip.new_slot_end")))
		cfg->new_slot_at_right = strcasecmp(s, "left") != 0;
	get_bool(root, "strip.wrap_around", &cfg->wrap_around);
	if ((s = get_str(root, "strip.close_focus"))) {
		if (!strcasecmp(s, "right")) cfg->close_focus = CLOSE_RIGHT;
		else if (!strcasecmp(s, "recent")) cfg->close_focus = CLOSE_RECENT;
		else cfg->close_focus = CLOSE_LEFT;
	}
	cfg->focus_width = clampf(cfg->focus_width, 0.3f, 1.0f);
	cfg->peek = clampf(cfg->peek, 0.0f, 0.3f);

	get_bool(root, "animation.enabled", &cfg->anim_enabled);
	get_int(root, "animation.duration_ms", &cfg->anim_ms);
	if (cfg->anim_ms < 0) cfg->anim_ms = 0;
	if (cfg->anim_ms > 3000) cfg->anim_ms = 3000;
	if ((s = get_str(root, "animation.easing"))) {
		if (!strcasecmp(s, "cubic-out")) cfg->anim_easing = EASE_CUBIC_OUT;
		else if (!strcasecmp(s, "quad-out")) cfg->anim_easing = EASE_QUAD_OUT;
		else if (!strcasecmp(s, "linear")) cfg->anim_easing = EASE_LINEAR;
		else if (!strcasecmp(s, "expo-out")) cfg->anim_easing = EASE_EXPO_OUT;
		else if (!strcasecmp(s, "quart-out")) cfg->anim_easing = EASE_QUART_OUT;
		else if (!strcasecmp(s, "quint-out")) cfg->anim_easing = EASE_QUINT_OUT;
		else if (!strcasecmp(s, "sine-out")) cfg->anim_easing = EASE_SINE_OUT;
		else fprintf(stderr, "monolith: config: unknown animation.easing \"%s\"\n", s);
	}
	toml_datum_t bz = toml_seek(root, "animation.bezier");
	if (bz.type == TOML_ARRAY && bz.u.arr.size == 4) {
		for (int i = 0; i < 4; i++) {
			toml_datum_t e = bz.u.arr.elem[i];
			double v = e.type == TOML_FP64 ? e.u.fp64 : e.type == TOML_INT64 ? (double)e.u.int64 : 0.0;
			if (i % 2 == 0) v = v < 0 ? 0 : v > 1 ? 1 : v; /* x control points must stay in 0..1 */
			cfg->anim_bez[i] = (float)v;
		}
		cfg->anim_easing = EASE_BEZIER; /* an explicit curve beats the preset name */
	}
	get_float(root, "animation.jump_start", &cfg->anim_jump);
	cfg->anim_jump = clampf(cfg->anim_jump, 0.0f, 0.9f);
	get_float(root, "look.tilt_degrees", &cfg->look_tilt);
	get_float(root, "look.scale", &cfg->look_scale);
	get_float(root, "look.dim", &cfg->look_dim);
	get_float(root, "look.perspective", &cfg->look_persp);
	cfg->look_tilt = clampf(cfg->look_tilt, -80.0f, 80.0f);
	cfg->look_scale = clampf(cfg->look_scale, 0.3f, 1.0f);
	cfg->look_dim = clampf(cfg->look_dim, 0.0f, 0.9f);
	cfg->look_persp = clampf(cfg->look_persp, 0.8f, 6.0f);

	if ((s = get_str(root, "windows_mode.popups")))
		cfg->popups_auto = strcasecmp(s, "off") != 0;
	if ((s = get_str(root, "windows_mode.same_client")))
		cfg->same_client_join = strcasecmp(s, "new") != 0;
	get_bool(root, "windows_mode.revert_when_single", &cfg->revert_when_single);
	toml_datum_t ds = toml_seek(root, "windows_mode.default_size");
	if (ds.type == TOML_ARRAY && ds.u.arr.size == 2) {
		toml_datum_t a = ds.u.arr.elem[0], b = ds.u.arr.elem[1];
		double w = a.type == TOML_FP64 ? a.u.fp64 : (double)a.u.int64;
		double h = b.type == TOML_FP64 ? b.u.fp64 : (double)b.u.int64;
		cfg->default_w = clampf((float)w, 0.1f, 1.0f);
		cfg->default_h = clampf((float)h, 0.1f, 1.0f);
	}
	get_int(root, "windows_mode.margin", &cfg->wm_margin);

	parse_input(root, "input.touchpad", &cfg->touchpad);
	parse_input(root, "input.mouse", &cfg->mouse);
	get_int(root, "gestures.swipe_fingers", &cfg->swipe_fingers);
	get_float(root, "gestures.swipe_distance", &cfg->swipe_distance);
	get_float(root, "gestures.swipe_commit", &cfg->swipe_commit);
	get_bool(root, "gestures.swipe_natural", &cfg->swipe_natural);
	cfg->swipe_distance = clampf(cfg->swipe_distance, 0.1f, 2.0f);
	cfg->swipe_commit = clampf(cfg->swipe_commit, 0.05f, 0.9f);

	get_bool(root, "sound.enabled", &cfg->sound_enabled);
	get_int(root, "sound.cooldown_ms", &cfg->sound_cooldown_ms);
	if ((s = get_str(root, "sound.overlap"))) {
		if (!strcasecmp(s, "allow")) cfg->sound_overlap = OVERLAP_ALLOW;
		else if (!strcasecmp(s, "drop")) cfg->sound_overlap = OVERLAP_DROP;
		else cfg->sound_overlap = OVERLAP_RESTART;
	}
	static const char *snd_keys[SND_COUNT] = {
		"sound.events.slot_switch", "sound.events.slot_open",
		"sound.events.slot_close", "sound.events.windows_mode",
	};
	for (int i = 0; i < SND_COUNT; i++) {
		toml_datum_t d = toml_seek(root, snd_keys[i]);
		if (d.type != TOML_UNKNOWN) {
			free_strv(cfg->sound_cmd[i]);
			cfg->sound_cmd[i] = sound_cmd_from(d, snd_keys[i]); /* [] => NULL => silent */
		}
	}

	/* Events the config does not mention fall back to the bundled sounds, if installed.
	 * An explicit [] still means silent. */
	static const char *snd_files[SND_COUNT] = { "switch.wav", "open.wav", NULL, "mode.wav" };
	for (int i = 0; i < SND_COUNT; i++) {
		if (!snd_files[i] || cfg->sound_cmd[i] || toml_seek(root, snd_keys[i]).type != TOML_UNKNOWN) continue;
		const char *home = getenv("HOME");
		char path[1024];
		const char *dirs[] = { "/usr/local/share/monolith/sounds", "/usr/share/monolith/sounds", NULL };
		for (int k = 0; k < 3; k++) {
			if (k == 2) { if (!home) break; snprintf(path, sizeof path, "%s/.local/share/monolith/sounds/%s", home, snd_files[i]); }
			else snprintf(path, sizeof path, "%s/%s", dirs[k], snd_files[i]);
			if (access(path, R_OK) != 0) continue;
			char **v = calloc(4, sizeof *v);
			if (!v) abort();
			v[0] = xstrdup("aplay"); v[1] = xstrdup("-q"); v[2] = xstrdup(path);
			cfg->sound_cmd[i] = v;
			break;
		}
	}

	if ((s = get_str(root, "background.color"))) parse_color(s, cfg->bg_color);

	set_str(&cfg->cursor_theme, get_str(root, "cursor.theme"));
	get_int(root, "cursor.size", &cfg->cursor_size);

	set_str(&cfg->kb_layout, get_str(root, "keyboard.layout"));
	set_str(&cfg->kb_variant, get_str(root, "keyboard.variant"));
	set_str(&cfg->kb_options, get_str(root, "keyboard.options"));
	get_int(root, "keyboard.repeat_rate", &cfg->repeat_rate);
	get_int(root, "keyboard.repeat_delay", &cfg->repeat_delay);

	/* [keybinds]: user entries are appended after the defaults and win,
	 * because lookups scan from the end. */
	toml_datum_t kb = toml_seek(root, "keybinds");
	if (kb.type == TOML_TABLE) {
		for (int i = 0; i < kb.u.tab.size; i++) {
			toml_datum_t v = kb.u.tab.value[i];
			if (v.type != TOML_STRING) continue;
			char *spec = strndup(kb.u.tab.key[i], (size_t)kb.u.tab.len[i]);
			if (!spec) abort();
			add_bind(cfg, spec, v.u.s);
			free(spec);
		}
	}

	toml_datum_t rules = toml_seek(root, "rules");
	if (rules.type == TOML_ARRAY) {
		for (int i = 0; i < rules.u.arr.size; i++) {
			toml_datum_t r = rules.u.arr.elem[i];
			const char *app = get_str(r, "app_id");
			if (!app) continue;
			struct rule *nr = realloc(cfg->rules, (cfg->n_rules + 1) * sizeof *nr);
			if (!nr) abort();
			cfg->rules = nr;
			struct rule *rr = &cfg->rules[cfg->n_rules++];
			rr->app_id = xstrdup(app);
			rr->windows_mode = false;
			rr->new_slot = false;
			get_bool(r, "windows_mode", &rr->windows_mode);
			const char *sl = get_str(r, "slot");
			if (sl && !strcasecmp(sl, "new")) rr->new_slot = true;
		}
	}

	toml_datum_t as = toml_seek(root, "autostart");
	if (as.type == TOML_ARRAY) {
		for (int i = 0; i < as.u.arr.size; i++) {
			char **argv = strv_from(toml_seek(as.u.arr.elem[i], "command"));
			if (!argv) continue;
			char ***na = realloc(cfg->autostart, (cfg->n_autostart + 1) * sizeof *na);
			if (!na) abort();
			cfg->autostart = na;
			cfg->autostart[cfg->n_autostart++] = argv;
		}
	}
}

bool config_load(const char *path, struct config *out, char *err, size_t errlen) {
	toml_result_t res = toml_parse_file_ex(path);
	if (!res.ok) {
		snprintf(err, errlen, "%s: %s", path, res.errmsg);
		toml_free(res);
		return false;
	}
	struct config cfg;
	config_defaults(&cfg);
	apply(&cfg, res.toptab);
	toml_free(res);
	*out = cfg;
	return true;
}

char *config_find_path(void) {
	char buf[1024];
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	if (xdg && *xdg) {
		snprintf(buf, sizeof buf, "%s/monolith/config.toml", xdg);
		if (access(buf, R_OK) == 0) return xstrdup(buf);
	} else if (home && *home) {
		snprintf(buf, sizeof buf, "%s/.config/monolith/config.toml", home);
		if (access(buf, R_OK) == 0) return xstrdup(buf);
	}
	if (access("/etc/monolith/config.toml", R_OK) == 0)
		return xstrdup("/etc/monolith/config.toml");
	return NULL;
}
