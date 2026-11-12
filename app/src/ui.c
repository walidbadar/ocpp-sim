/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 *
 * A charging station display: a top bar (station, connector, status, time)
 * over one screen that follows what the selected connector is doing.
 *
 *   Ready        nothing plugged in: the socket to plug into, the power to
 *                offer, the card reader
 *   Connected    cable in or card accepted: power, EV and EVSE ready, start
 *   Charging     transaction running: energy, power, current, voltage, stop
 *   Complete     transaction over, cable still in
 *   Out of order fault, or taken out of service
 *   Diagnostics  link to the central system, backend URL, identity, the
 *                fault and service switches of the connector
 *
 * All sizes are given for 800x480 and scaled to the width of the display.
 * Headlines and numbers use Montserrat Bold through LVGL's TinyTTF, the
 * rest and the icons the built-in fonts.
 */

#include "ui.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/util.h>

#include <lvgl.h>

#include "config.h"
#include "cp.h"
#include "url.h"

/* ---- palette ------------------------------------------------------------ */

#define C_BG_TOP 0xf7f7f7
#define C_BG_BOT 0xe8e9ea
#define C_WATER  0xfcfcfc /* the word behind the Ready screen */
#define C_INK    0x14181b
#define C_INK_2  0x2a3036 /* raised parts on ink */
#define C_GREY   0x868d94
#define C_SOFT   0xb4bac0
#define C_CARD   0xfcfcfc
#define C_TRACK  0xdfe3e6
#define C_GREEN  0x12b76a
#define C_MINT   0x34d17f /* green on ink */
#define C_AMBER  0xd97706
#define C_RED    0xdc2626
#define C_WHITE  0xffffff

/* ---- fonts and scale ------------------------------------------------------ */

static const uint8_t montserrat_bold[] = {
#include "montserrat_bold.ttf.inc"
};

static struct {
	int w;
	int h;
	/* Montserrat Bold */
	lv_font_t *brand;
	lv_font_t *h1;
	lv_font_t *h2;
	lv_font_t *num;
	lv_font_t *hero;
	lv_font_t *water;
	/* built in: medium weight and the symbols */
	const lv_font_t *body;
	const lv_font_t *small;
	const lv_font_t *icon;
	const lv_font_t *icon_l;
} lay;

/* A length of the 800x480 design on this display */
static int px(int v)
{
	return (v * lay.w + 400) / 800;
}

static lv_font_t *bold(int size)
{
	return lv_tiny_ttf_create_data_ex(montserrat_bold, sizeof(montserrat_bold), px(size),
					  LV_FONT_KERNING_NONE, 96);
}

/* ---- state ---------------------------------------------------------------- */

enum view {
	VIEW_READY,
	VIEW_CONNECTED,
	VIEW_CHARGING,
	VIEW_COMPLETE,
	VIEW_FAULT,
	VIEW_DIAG,
	VIEW_COUNT
};

#define PRESETS 3
#define PINS    7
#define CHECKS  4

/* A power level to offer, as a card (Ready) and as a row (Connected) */
struct preset_ui {
	lv_obj_t *box;
	lv_obj_t *icon;
	lv_obj_t *name;
	lv_obj_t *num;
	lv_obj_t *unit;
	lv_obj_t *caption;
	lv_obj_t *radio; /* rows only */
};

/* The face of a Type 2 socket, drawn from circles */
struct socket_ui {
	lv_obj_t *ring;
	lv_obj_t *pin[PINS];
};

struct stat_ui {
	lv_obj_t *value;
	lv_obj_t *unit;
};

static int sel = 1; /* connector on display */
static bool diag_open;
static enum view current_view = VIEW_COUNT;
static atomic_t requested_page = ATOMIC_INIT(-1);
static atomic_t requested_connector = ATOMIC_INIT(0);
static uint32_t preset_w[PRESETS];

static lv_obj_t *views[VIEW_COUNT];

/* top bar */
static lv_obj_t *t_station;
static lv_obj_t *t_conn[CP_CONNECTORS];
static lv_obj_t *t_conn_label[CP_CONNECTORS];
static lv_obj_t *t_link_icon;
static lv_obj_t *t_dot;
static lv_obj_t *t_status;
static lv_obj_t *t_clock;

/* ready */
static lv_obj_t *r_chip;
static lv_obj_t *r_msg;
static struct preset_ui r_preset[PRESETS];
static struct socket_ui r_socket;
static lv_obj_t *r_qr;
static char r_qr_text[EVSE_IDTAG_MAX + 1];

/* connected */
static lv_obj_t *c_headline;
static lv_obj_t *c_sub;
static struct preset_ui c_preset[PRESETS];
static lv_obj_t *c_idtag;
static lv_obj_t *c_msg;
static lv_obj_t *c_ev;
static lv_obj_t *c_evse;
static lv_obj_t *c_start;
static lv_obj_t *c_start_label;
static lv_obj_t *c_plug_label;

/* charging */
static lv_obj_t *g_icon;
static lv_obj_t *g_headline;
static lv_obj_t *g_sub;
static lv_obj_t *g_hero;
static lv_obj_t *g_hero_unit;
static struct socket_ui g_socket;
static struct stat_ui g_stat[4];
static lv_obj_t *g_ev_icon;
static lv_obj_t *g_evse_icon;
static lv_obj_t *g_bar;

/* complete */
static lv_obj_t *d_sub;
static struct stat_ui d_stat[4];
static lv_obj_t *d_meter;
static lv_obj_t *d_button_label;

/* out of order */
static lv_obj_t *f_headline;
static lv_obj_t *f_sub;

/* diagnostics */
static lv_obj_t *x_sub;
static lv_obj_t *x_cbid;
static lv_obj_t *x_qr;
static char x_qr_text[BACKEND_URL_HOST_MAX + BACKEND_URL_PATH_MAX + 96];
static lv_obj_t *x_check_box[CHECKS];
static lv_obj_t *x_check_icon[CHECKS];
static lv_obj_t *x_check_text[CHECKS];
static lv_obj_t *x_check_state[CHECKS];
static lv_obj_t *x_url;
static lv_obj_t *x_hint;
static lv_obj_t *x_conn_title;
static lv_obj_t *x_limit;
static lv_obj_t *x_limit_bar;
static lv_obj_t *x_fault;
static lv_obj_t *x_service;
static lv_obj_t *x_idtag;

/* text editor */
static lv_obj_t *editor;
static lv_obj_t *editor_title;
static lv_obj_t *editor_ta;
static lv_obj_t *editor_msg;
static int (*editor_done)(const char *text);

/* ---- small builders ------------------------------------------------------- */

static lv_color_t col(uint32_t rgb)
{
	return lv_color_hex(rgb);
}

/* A plain container: no theme styles, not scrollable, sized by its content */
static lv_obj_t *box(lv_obj_t *parent, lv_flex_flow_t flow, int pad, int gap)
{
	lv_obj_t *o = lv_obj_create(parent);

	lv_obj_remove_style_all(o);
	lv_obj_set_size(o, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(o, flow);
	lv_obj_set_style_pad_all(o, pad, 0);
	lv_obj_set_style_pad_row(o, gap, 0);
	lv_obj_set_style_pad_column(o, gap, 0);
	lv_obj_set_scrollable(o, false);
	lv_obj_set_clickable(o, false);

	return o;
}

/* Let the shadows of the children show around a container that draws nothing
 * itself: LVGL clips children to the parent, extended by what the parent
 * says it draws outside its own box.
 */
static void spill(lv_obj_t *o)
{
	lv_obj_set_overflow_visible(o, true);
	lv_obj_set_style_transform_width(o, px(40), 0);
	lv_obj_set_style_transform_height(o, px(40), 0);
}

static lv_obj_t *row(lv_obj_t *parent, int gap)
{
	lv_obj_t *r = box(parent, LV_FLEX_FLOW_ROW, 0, gap);

	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	return r;
}

static lv_obj_t *label(lv_obj_t *parent, const char *txt, const lv_font_t *font, uint32_t rgb)
{
	lv_obj_t *l = lv_label_create(parent);

	lv_label_set_text(l, txt);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, col(rgb), 0);

	return l;
}

/* Labels are only touched when their text changes: no needless redraws */
static void set_text(lv_obj_t *l, const char *txt)
{
	if (strcmp(lv_label_get_text(l), txt) != 0) {
		lv_label_set_text(l, txt);
	}
}

static void set_ink(lv_obj_t *l, uint32_t rgb)
{
	lv_obj_set_style_text_color(l, col(rgb), 0);
}

/* Background, rounded corners and, for raised things, a soft shadow */
static void surface(lv_obj_t *o, uint32_t rgb, int radius, bool raised)
{
	lv_obj_set_style_bg_color(o, col(rgb), 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(o, radius, 0);
	if (raised) {
		lv_obj_set_style_shadow_width(o, px(26), 0);
		lv_obj_set_style_shadow_offset_y(o, px(8), 0);
		lv_obj_set_style_shadow_color(o, col(0x000000), 0);
		lv_obj_set_style_shadow_opa(o, LV_OPA_10, 0);
	}
}

static void on_click(lv_obj_t *o, lv_event_cb_t cb, void *arg)
{
	lv_obj_set_clickable(o, true);
	lv_obj_set_style_opa(o, LV_OPA_70, LV_STATE_PRESSED);
	lv_obj_add_event_cb(o, cb, LV_EVENT_CLICKED, arg);
}

static lv_obj_t *card(lv_obj_t *parent, int pad, int gap)
{
	lv_obj_t *c = box(parent, LV_FLEX_FLOW_COLUMN, pad, gap);

	surface(c, C_CARD, px(18), true);

	return c;
}

/* A pill shaped button; the text is returned through @p text */
static lv_obj_t *pill(lv_obj_t *parent, const char *txt, uint32_t bg, uint32_t fg,
		      lv_event_cb_t cb, lv_obj_t **text)
{
	lv_obj_t *b = box(parent, LV_FLEX_FLOW_ROW, 0, 0);
	lv_obj_t *l;

	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	surface(b, bg, px(16), bg != C_CARD);
	on_click(b, cb, NULL);
	l = label(b, txt, lay.brand, fg);
	if (text != NULL) {
		*text = l;
	}

	return b;
}

static lv_obj_t *round_button(lv_obj_t *parent, int size, const char *symbol, lv_event_cb_t cb,
			      lv_obj_t **icon)
{
	lv_obj_t *b = box(parent, LV_FLEX_FLOW_ROW, 0, 0);
	lv_obj_t *l;

	lv_obj_set_size(b, size, size);
	lv_obj_set_flex_align(b, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	surface(b, C_CARD, LV_RADIUS_CIRCLE, true);
	on_click(b, cb, NULL);
	l = label(b, symbol, lay.icon, C_INK);
	if (icon != NULL) {
		*icon = l;
	}

	return b;
}

static lv_obj_t *toggle(lv_obj_t *parent, lv_event_cb_t cb)
{
	lv_obj_t *s = lv_switch_create(parent);

	lv_obj_set_size(s, px(46), px(26));
	lv_obj_set_style_bg_color(s, col(C_TRACK), 0);
	lv_obj_set_style_bg_color(s, col(C_GREEN), LV_PART_INDICATOR | LV_STATE_CHECKED);
	lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);

	return s;
}

/* "Caption ......... [switch]" */
static lv_obj_t *toggle_row(lv_obj_t *parent, const char *caption, lv_event_cb_t cb)
{
	lv_obj_t *r = row(parent, px(8));
	lv_obj_t *l;

	lv_obj_set_width(r, lv_pct(100));
	l = label(r, caption, lay.body, C_INK);
	lv_obj_set_flex_grow(l, 1);

	return toggle(r, cb);
}

static lv_obj_t *bar(lv_obj_t *parent, int height)
{
	lv_obj_t *b = lv_bar_create(parent);

	lv_obj_remove_style_all(b);
	lv_obj_set_height(b, height);
	lv_obj_set_style_radius(b, height / 2, 0);
	lv_obj_set_style_bg_color(b, col(C_TRACK), 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_radius(b, height / 2, LV_PART_INDICATOR);
	lv_obj_set_style_bg_color(b, col(C_GREEN), LV_PART_INDICATOR);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, LV_PART_INDICATOR);
	lv_bar_set_range(b, 0, 1000);

	return b;
}

static lv_obj_t *circle(lv_obj_t *parent, int size, uint32_t rgb)
{
	lv_obj_t *c = lv_obj_create(parent);

	lv_obj_remove_style_all(c);
	lv_obj_set_size(c, size, size);
	lv_obj_set_scrollable(c, false);
	lv_obj_set_clickable(c, false);
	surface(c, rgb, LV_RADIUS_CIRCLE, false);

	return c;
}

static lv_obj_t *qr_code(lv_obj_t *parent, int size)
{
	lv_obj_t *qr = lv_qrcode_create(parent);

	lv_qrcode_set_size(qr, size);
	lv_qrcode_set_dark_color(qr, col(C_INK));
	lv_qrcode_set_light_color(qr, col(C_CARD));

	return qr;
}

static void socket_build(struct socket_ui *s, lv_obj_t *parent, int d)
{
	/* x, y, radius in thousandths of the diameter: PP and CP on top,
	 * N, PE and L1 across the middle, L3 and L2 below
	 */
	static const int16_t pins[PINS][3] = {
		{-150, -235, 52}, {150, -235, 52}, {-285, -10, 78}, {0, -10, 78},
		{285, -10, 78},   {-150, 225, 78}, {150, 225, 78},
	};
	lv_obj_t *face;

	s->ring = circle(parent, d, C_INK);
	lv_obj_set_style_border_width(s->ring, MAX(d / 22, 2), 0);
	lv_obj_set_style_border_color(s->ring, col(C_INK_2), 0);
	lv_obj_set_style_shadow_width(s->ring, px(30), 0);
	lv_obj_set_style_shadow_offset_y(s->ring, px(10), 0);
	lv_obj_set_style_shadow_color(s->ring, col(0x000000), 0);
	lv_obj_set_style_shadow_opa(s->ring, LV_OPA_20, 0);

	face = circle(s->ring, d * 80 / 100, C_INK_2);
	lv_obj_center(face);

	for (int i = 0; i < PINS; i++) {
		s->pin[i] = circle(s->ring, d * pins[i][2] * 2 / 1000, C_INK);
		lv_obj_align(s->pin[i], LV_ALIGN_CENTER, d * pins[i][0] / 1000,
			     d * pins[i][1] / 1000);
	}
}

static void socket_set(struct socket_ui *s, uint32_t ring, uint32_t pins)
{
	lv_obj_set_style_border_color(s->ring, col(ring), 0);
	for (int i = 0; i < PINS; i++) {
		lv_obj_set_style_bg_color(s->pin[i], col(pins), 0);
	}
}

/* "42 kW" over "Power": value and unit are returned for the refresh */
static void stat_build(struct stat_ui *s, lv_obj_t *parent, const char *caption)
{
	lv_obj_t *c = box(parent, LV_FLEX_FLOW_COLUMN, 0, 0);
	lv_obj_t *r = box(c, LV_FLEX_FLOW_ROW, 0, px(4));

	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	s->value = label(r, "0", lay.h2, C_INK);
	s->unit = label(r, "", lay.small, C_GREY);
	lv_obj_set_style_pad_bottom(s->unit, px(4), 0);
	label(c, caption, lay.small, C_GREY);
}

static void stat_set(struct stat_ui *s, const char *value, const char *unit)
{
	set_text(s->value, value);
	set_text(s->unit, unit);
}

/* ---- formatting ----------------------------------------------------------- */

static void fmt_kw(char *buf, size_t len, uint32_t watts)
{
	snprintf(buf, len, "%u.%u", watts / 1000, (watts % 1000) / 100);
}

static void fmt_duration(char *buf, size_t len, uint32_t seconds)
{
	if (seconds >= 3600) {
		snprintf(buf, len, "%uh %02um", seconds / 3600, (seconds % 3600) / 60);
	} else {
		snprintf(buf, len, "%u:%02u", seconds / 60, seconds % 60);
	}
}

/* Energy as "842" "Wh" below a kilowatt hour, "1.25" "kWh" from there */
static const char *fmt_energy(char *buf, size_t len, uint32_t wh)
{
	if (wh < 1000) {
		snprintf(buf, len, "%u", wh);
		return "Wh";
	}
	snprintf(buf, len, "%u.%02u", wh / 1000, (wh % 1000) / 10);

	return "kWh";
}

/* ---- text editor ---------------------------------------------------------- */

static void editor_close(void)
{
	lv_obj_set_hidden(editor, true);
	editor_done = NULL;
}

static void editor_event(lv_event_t *e)
{
	char msg[48];
	int ret;

	if (lv_event_get_code(e) == LV_EVENT_CANCEL) {
		editor_close();
		return;
	}
	if (editor_done == NULL) {
		return;
	}

	ret = editor_done(lv_textarea_get_text(editor_ta));
	if (ret == 0) {
		editor_close();
		ui_refresh();
		return;
	}

	/* keep the text so that it can be corrected */
	snprintf(msg, sizeof(msg), "Not accepted (%d)", ret);
	lv_label_set_text(editor_msg, msg);
}

static void editor_build(lv_obj_t *screen)
{
	lv_obj_t *kb;

	/* over the whole screen, outside its layout */
	editor = box(screen, LV_FLEX_FLOW_COLUMN, px(24), px(10));
	lv_obj_set_ignore_layout(editor, true);
	lv_obj_set_pos(editor, 0, 0);
	lv_obj_set_size(editor, lay.w, lay.h);
	surface(editor, C_BG_TOP, 0, false);
	lv_obj_set_clickable(editor, true);

	editor_title = label(editor, "", lay.h2, C_INK);

	editor_ta = lv_textarea_create(editor);
	lv_textarea_set_one_line(editor_ta, true);
	lv_obj_set_width(editor_ta, lv_pct(100));
	lv_obj_set_style_text_font(editor_ta, lay.body, 0);
	lv_obj_set_style_radius(editor_ta, px(12), 0);

	editor_msg = label(editor, "", lay.small, C_RED);

	kb = lv_keyboard_create(editor);
	lv_obj_set_ignore_layout(kb, true);
	lv_obj_set_size(kb, lay.w, lay.h * 55 / 100);
	lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_text_font(kb, lay.body, 0);
	lv_keyboard_set_textarea(kb, editor_ta);
	lv_obj_add_event_cb(kb, editor_event, LV_EVENT_READY, NULL);
	lv_obj_add_event_cb(kb, editor_event, LV_EVENT_CANCEL, NULL);

	lv_obj_set_hidden(editor, true);
}

static void editor_open(const char *title, const char *value, uint32_t max_len,
			int (*done)(const char *text))
{
	lv_label_set_text(editor_title, title);
	lv_label_set_text(editor_msg, "");
	lv_textarea_set_max_length(editor_ta, max_len);
	lv_textarea_set_text(editor_ta, value);
	editor_done = done;
	lv_obj_set_hidden(editor, false);
}

static int idtag_edited(const char *value)
{
	return config_set_idtag(sel, value);
}

static void idtag_clicked(lv_event_t *e)
{
	struct app_config cfg;
	char title[48];

	ARG_UNUSED(e);

	config_get(&cfg);
	snprintf(title, sizeof(title), "Card of connector %d", sel);
	editor_open(title, cfg.idtag[sel - 1], EVSE_IDTAG_MAX, idtag_edited);
}

static void url_clicked(lv_event_t *e)
{
	struct app_config cfg;

	ARG_UNUSED(e);

	config_get(&cfg);
	editor_open("Backend URL", cfg.url, CONFIG_URL_MAX, config_set_url);
}

static void cbid_clicked(lv_event_t *e)
{
	struct app_config cfg;

	ARG_UNUSED(e);

	config_get(&cfg);
	editor_open("Charge box ID", cfg.cbid, CONFIG_CBID_MAX, config_set_cbid);
}

/* ---- what the buttons do -------------------------------------------------- */

static void plug_clicked(lv_event_t *e)
{
	struct cp_connector c;

	ARG_UNUSED(e);

	cp_get_connector(sel, &c);
	cp_set_plugged(sel, !c.ev_plugged);
	ui_refresh();
}

static void unplug_clicked(lv_event_t *e)
{
	ARG_UNUSED(e);

	cp_set_plugged(sel, false);
	ui_refresh();
}

static void card_clicked(lv_event_t *e)
{
	ARG_UNUSED(e);

	(void)cp_present_tag(sel, NULL);
	ui_refresh();
}

/* Back to Ready: the card withdrawn and the cable out */
static void back_clicked(lv_event_t *e)
{
	ARG_UNUSED(e);

	(void)cp_stop(sel);
	cp_set_plugged(sel, false);
	ui_refresh();
}

static void stop_clicked(lv_event_t *e)
{
	ARG_UNUSED(e);

	(void)cp_stop(sel);
	ui_refresh();
}

static void preset_clicked(lv_event_t *e)
{
	cp_set_limit(sel, preset_w[(uintptr_t)lv_event_get_user_data(e)]);
	ui_refresh();
}

static void ev_toggled(lv_event_t *e)
{
	struct cp_connector c;

	ARG_UNUSED(e);

	cp_get_connector(sel, &c);
	cp_set_ev_ready(sel, !c.ev_ready);
	ui_refresh();
}

static void evse_toggled(lv_event_t *e)
{
	struct cp_connector c;

	ARG_UNUSED(e);

	cp_get_connector(sel, &c);
	cp_set_evse_ready(sel, !c.evse_ready);
	ui_refresh();
}

static void fault_toggled(lv_event_t *e)
{
	cp_set_fault(sel, lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
	ui_refresh();
}

static void service_toggled(lv_event_t *e)
{
	cp_set_inoperative(sel, !lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
	ui_refresh();
}

static void connector_clicked(lv_event_t *e)
{
	sel = (int)(uintptr_t)lv_event_get_user_data(e);
	ui_refresh();
}

static void diag_clicked(lv_event_t *e)
{
	ARG_UNUSED(e);

	diag_open = !diag_open;
	ui_refresh();
}

static void restart_clicked(lv_event_t *e)
{
	ARG_UNUSED(e);

	sys_reboot(SYS_REBOOT_COLD);
}

/* ---- top bar -------------------------------------------------------------- */

static void topbar_build(lv_obj_t *screen)
{
	lv_obj_t *top = row(screen, px(10));
	lv_obj_t *left, *right, *seg, *status;

	lv_obj_set_size(top, lv_pct(100), px(64));
	lv_obj_set_style_pad_hor(top, px(24), 0);
	lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);

	left = row(top, px(8));
	label(left, LV_SYMBOL_CHARGE, lay.icon, C_INK);
	label(left, "OCPP SIM", lay.brand, C_INK);
	t_station = label(left, "", lay.body, C_GREY);
	lv_obj_set_style_pad_left(t_station, px(4), 0);

	right = row(top, px(10));
	spill(right);

	/* which connector the screen shows */
	if (CP_CONNECTORS > 1) {
		seg = row(right, px(2));
		lv_obj_set_style_pad_all(seg, px(3), 0);
		surface(seg, C_CARD, LV_RADIUS_CIRCLE, true);
		for (int i = 0; i < CP_CONNECTORS; i++) {
			char n[4];

			snprintf(n, sizeof(n), "%d", i + 1);
			t_conn[i] = box(seg, LV_FLEX_FLOW_ROW, 0, 0);
			lv_obj_set_size(t_conn[i], px(30), px(26));
			lv_obj_set_flex_align(t_conn[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
					      LV_FLEX_ALIGN_CENTER);
			surface(t_conn[i], C_INK, LV_RADIUS_CIRCLE, false);
			on_click(t_conn[i], connector_clicked, (void *)(uintptr_t)(i + 1));
			t_conn_label[i] = label(t_conn[i], n, lay.body, C_INK);
		}
	}

	/* link to the central system, and the way into the diagnostics */
	round_button(right, px(32), LV_SYMBOL_WIFI, diag_clicked, &t_link_icon);
	lv_obj_set_style_text_font(t_link_icon, lay.small, 0);

	status = row(right, px(7));
	lv_obj_set_style_pad_hor(status, px(12), 0);
	lv_obj_set_style_pad_ver(status, px(6), 0);
	surface(status, C_CARD, LV_RADIUS_CIRCLE, true);
	t_dot = circle(status, px(8), C_GREEN);
	t_status = label(status, "", lay.body, C_INK);

	t_clock = label(right, "00:00", lay.h2, C_INK);
}

static void topbar_refresh(const struct cp_status *st, const struct app_config *cfg,
			   const struct cp_connector *c)
{
	uint32_t dot;
	struct timespec ts;
	uint32_t day_s;
	char buf[CONFIG_CBID_MAX + 16];

	snprintf(buf, sizeof(buf), "Station  %s", cfg->cbid);
	set_text(t_station, buf);

	if (CP_CONNECTORS > 1) {
		for (int i = 0; i < CP_CONNECTORS; i++) {
			bool on = i + 1 == sel;

			lv_obj_set_style_bg_opa(t_conn[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
			set_ink(t_conn_label[i], on ? C_WHITE : C_GREY);
		}
	}

	set_ink(t_link_icon, st->link == CP_LINK_ONLINE         ? C_GREEN
			     : st->link == CP_LINK_UNCONFIGURED ? C_SOFT
								: C_AMBER);

	if (diag_open) {
		dot = C_AMBER;
		set_text(t_status, "Service");
	} else {
		switch (c->status) {
		case EVSE_STATUS_SUSPENDED_EV:
		case EVSE_STATUS_SUSPENDED_EVSE:
			dot = C_AMBER;
			break;
		case EVSE_STATUS_FAULTED:
			dot = C_RED;
			break;
		case EVSE_STATUS_UNAVAILABLE:
			dot = C_SOFT;
			break;
		default:
			dot = C_GREEN;
			break;
		}
		set_text(t_status, evse_status_str(c->status));
	}
	lv_obj_set_style_bg_color(t_dot, col(dot), 0);

	/* time of day, UTC */
	sys_clock_gettime(SYS_CLOCK_REALTIME, &ts);
	day_s = (uint32_t)(ts.tv_sec % 86400);
	snprintf(buf, sizeof(buf), "%02u:%02u", day_s / 3600, (day_s % 3600) / 60);
	set_text(t_clock, buf);
}

/* ---- power presets -------------------------------------------------------- */

static const char *const preset_names[PRESETS] = {"FAST", "STANDARD", "ECO"};
static const char *const preset_icons[PRESETS] = {LV_SYMBOL_CHARGE, LV_SYMBOL_POWER,
						  LV_SYMBOL_TINT};

static void preset_caption(char *buf, size_t len, uint32_t watts)
{
	uint32_t volts = CONFIG_OCPP_SIM_APP_VOLTAGE_V * CONFIG_OCPP_SIM_APP_PHASES;

	/* to the nearest ampere, as the ratings are given */
	snprintf(buf, len, "%d x %u A", CONFIG_OCPP_SIM_APP_PHASES, (watts + volts / 2) / volts);
}

/* The card of the Ready screen: name on top, the power large, amps below */
static void preset_card_build(struct preset_ui *p, lv_obj_t *parent, int i)
{
	lv_obj_t *head, *value;
	char buf[24];

	p->box = card(parent, px(16), px(4));
	lv_obj_set_flex_grow(p->box, 1);
	lv_obj_set_height(p->box, lv_pct(100));
	lv_obj_set_flex_align(p->box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
			      LV_FLEX_ALIGN_START);
	on_click(p->box, preset_clicked, (void *)(uintptr_t)i);

	head = row(p->box, px(8));
	p->icon = label(head, preset_icons[i], lay.icon, C_INK);
	p->name = label(head, preset_names[i], lay.brand, C_INK);

	value = box(p->box, LV_FLEX_FLOW_ROW, 0, px(5));
	lv_obj_set_flex_align(value, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	fmt_kw(buf, sizeof(buf), preset_w[i]);
	p->num = label(value, buf, lay.num, C_INK);
	p->unit = label(value, "kW", lay.brand, C_GREEN);
	lv_obj_set_style_pad_bottom(p->unit, px(6), 0);

	preset_caption(buf, sizeof(buf), preset_w[i]);
	p->caption = label(p->box, buf, lay.small, C_GREY);
	p->radio = NULL;
}

/* The row of the Connected screen: icon, name and amps, power, radio button */
static void preset_row_build(struct preset_ui *p, lv_obj_t *parent, int i)
{
	lv_obj_t *names;
	char buf[24];

	p->box = row(parent, px(12));
	lv_obj_set_size(p->box, lv_pct(100), px(62));
	lv_obj_set_style_pad_hor(p->box, px(18), 0);
	surface(p->box, C_CARD, px(16), true);
	on_click(p->box, preset_clicked, (void *)(uintptr_t)i);

	p->icon = label(p->box, preset_icons[i], lay.icon, C_INK);

	names = box(p->box, LV_FLEX_FLOW_COLUMN, 0, px(1));
	lv_obj_set_flex_grow(names, 1);
	p->name = label(names, preset_names[i], lay.brand, C_INK);
	preset_caption(buf, sizeof(buf), preset_w[i]);
	p->caption = label(names, buf, lay.small, C_GREY);

	fmt_kw(buf, sizeof(buf), preset_w[i]);
	p->num = label(p->box, buf, lay.h2, C_INK);
	p->unit = label(p->box, "kW", lay.body, C_GREEN);

	p->radio = circle(p->box, px(18), C_MINT);
	lv_obj_set_style_border_color(p->radio, col(C_TRACK), 0);
}

static void preset_refresh(struct preset_ui *p, bool on)
{
	lv_obj_set_style_bg_color(p->box, col(on ? C_INK : C_CARD), 0);
	set_ink(p->icon, on ? C_MINT : C_INK);
	set_ink(p->name, on ? C_WHITE : C_INK);
	set_ink(p->num, on ? C_WHITE : C_INK);
	set_ink(p->unit, on ? C_MINT : C_GREEN);
	set_ink(p->caption, on ? C_SOFT : C_GREY);
	if (p->radio != NULL) {
		lv_obj_set_style_bg_opa(p->radio, on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(p->radio, on ? 0 : 2, 0);
	}
}

/* ---- Ready ---------------------------------------------------------------- */

static void ready_build(lv_obj_t *v)
{
	lv_obj_t *o, *r, *cards, *tap;

	o = label(v, "READY", lay.water, C_WATER);
	lv_obj_set_pos(o, px(-12), px(8));

	o = label(v, "Plug in, or tap a card to start", lay.h1, C_INK);
	lv_obj_set_pos(o, px(42), px(112));

	r = row(v, px(10));
	lv_obj_set_pos(r, px(42), px(158));
	label(r, "card", lay.body, C_GREY);
	o = row(r, px(8));
	lv_obj_set_style_pad_hor(o, px(12), 0);
	lv_obj_set_style_pad_ver(o, px(6), 0);
	surface(o, C_CARD, px(12), false);
	on_click(o, idtag_clicked, NULL);
	r_chip = label(o, "", lay.brand, C_INK);
	label(o, LV_SYMBOL_EDIT, lay.small, C_SOFT);
	r_msg = label(v, "", lay.small, C_RED);
	lv_obj_set_pos(r_msg, px(42), px(198));

	/* the socket stands where the charger would be: tap to plug in */
	socket_build(&r_socket, v, px(170));
	lv_obj_align(r_socket.ring, LV_ALIGN_TOP_RIGHT, px(-96), px(14));
	on_click(r_socket.ring, plug_clicked, NULL);
	o = label(v, "Tap the socket to plug in", lay.small, C_GREY);
	lv_obj_align(o, LV_ALIGN_TOP_RIGHT, px(-108), px(196));

	cards = row(v, px(12));
	spill(cards);
	lv_obj_set_size(cards, px(752), px(132));
	lv_obj_align(cards, LV_ALIGN_BOTTOM_LEFT, px(24), px(-22));
	for (int i = 0; i < PRESETS; i++) {
		preset_card_build(&r_preset[i], cards, i);
	}

	/* the card reader */
	tap = card(cards, px(10), px(4));
	lv_obj_set_size(tap, px(124), lv_pct(100));
	lv_obj_set_flex_align(tap, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
			      LV_FLEX_ALIGN_CENTER);
	on_click(tap, card_clicked, NULL);
	label(tap, "Tap card", lay.small, C_INK);
	r_qr = qr_code(tap, px(86));
}

static void ready_refresh(const struct cp_connector *c, const char *idtag)
{
	set_text(r_chip, idtag);
	/* a card that did not get through leaves the station ready */
	set_text(r_msg, c->auth == EVSE_AUTH_REJECTED ? "The card was refused by the central system"
			: c->auth == EVSE_AUTH_FAILED ? "No answer from the central system"
						      : "");
	for (int i = 0; i < PRESETS; i++) {
		preset_refresh(&r_preset[i], c->limit_w == preset_w[i]);
	}
	socket_set(&r_socket, C_INK_2, C_INK);

	/* the code a phone would scan is the card itself */
	if (strcmp(r_qr_text, idtag) != 0) {
		strcpy(r_qr_text, idtag);
		lv_qrcode_update(r_qr, idtag, strlen(idtag));
	}
}

/* ---- Connected ------------------------------------------------------------ */

static void connected_build(lv_obj_t *v)
{
	lv_obj_t *list, *side, *o, *r;

	o = round_button(v, px(36), LV_SYMBOL_LEFT, back_clicked, NULL);
	lv_obj_set_pos(o, px(34), px(2));
	lv_obj_set_style_shadow_width(o, 0, 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
	lv_obj_set_ext_click_area(o, px(6));

	c_headline = label(v, "", lay.h1, C_INK);
	lv_obj_set_pos(c_headline, px(42), px(40));
	c_sub = label(v, "", lay.body, C_GREY);
	lv_obj_set_pos(c_sub, px(42), px(84));

	list = box(v, LV_FLEX_FLOW_COLUMN, 0, px(12));
	spill(list);
	lv_obj_set_width(list, px(404));
	lv_obj_set_pos(list, px(42), px(118));
	for (int i = 0; i < PRESETS; i++) {
		preset_row_build(&c_preset[i], list, i);
	}

	side = card(v, px(20), px(10));
	lv_obj_set_size(side, px(284), px(374));
	lv_obj_align(side, LV_ALIGN_TOP_RIGHT, px(-42), px(12));

	label(side, "Card", lay.small, C_GREY);
	r = row(side, px(8));
	on_click(r, idtag_clicked, NULL);
	c_idtag = label(r, "", lay.h2, C_INK);
	label(r, LV_SYMBOL_EDIT, lay.small, C_SOFT);
	c_msg = label(side, "", lay.small, C_GREY);

	/* pushes the rest to the bottom of the card */
	o = box(side, LV_FLEX_FLOW_ROW, 0, 0);
	lv_obj_set_flex_grow(o, 1);

	label(side, "VEHICLE AND STATION", lay.small, C_SOFT);
	c_ev = toggle_row(side, "EV asks for energy", ev_toggled);
	c_evse = toggle_row(side, "EVSE offers energy", evse_toggled);

	c_start = pill(side, "Start charging", C_INK, C_WHITE, card_clicked, &c_start_label);
	lv_obj_set_size(c_start, lv_pct(100), px(50));
	lv_obj_set_style_margin_top(c_start, px(6), 0);

	o = pill(side, "Unplug", C_CARD, C_GREY, plug_clicked, &c_plug_label);
	lv_obj_set_size(o, lv_pct(100), px(30));
	lv_obj_set_style_text_font(c_plug_label, lay.body, 0);
}

static void connected_refresh(const struct cp_connector *c, const char *idtag)
{
	const char *start = "Start charging";
	uint32_t msg_ink = C_GREY;
	const char *msg = "";
	char buf[64];

	if (c->ev_plugged) {
		set_text(c_headline, "Cable connected");
	} else if (c->auth == EVSE_AUTH_ACCEPTED) {
		set_text(c_headline, "Card accepted, plug in");
	} else {
		set_text(c_headline, "Checking the card");
	}

	fmt_kw(buf, sizeof(buf), c->max_power_w);
	strcat(buf, " kW  " LV_SYMBOL_BULLET "  Type 2");
	set_text(c_sub, buf);

	for (int i = 0; i < PRESETS; i++) {
		preset_refresh(&c_preset[i], c->limit_w == preset_w[i]);
	}

	/* the card of the session once one was presented, else the one in hand */
	set_text(c_idtag, c->auth != EVSE_AUTH_NONE && c->idtag[0] != '\0' ? c->idtag : idtag);

	switch (c->auth) {
	case EVSE_AUTH_PENDING:
		msg = "Asking the central system...";
		start = "Checking...";
		break;
	case EVSE_AUTH_ACCEPTED:
		msg = c->ev_plugged ? "Accepted, starting the transaction"
				    : "Accepted, waiting for the cable";
		msg_ink = C_GREEN;
		start = c->ev_plugged ? "Starting..." : "Cancel";
		break;
	case EVSE_AUTH_REJECTED:
		msg = "Refused by the central system";
		msg_ink = C_RED;
		break;
	case EVSE_AUTH_FAILED:
		msg = "No answer from the central system";
		msg_ink = C_RED;
		break;
	default:
		msg = "Tap the button to present it";
		break;
	}
	set_ink(c_msg, msg_ink);
	set_text(c_msg, msg);
	set_text(c_start_label, start);

	lv_obj_set_state(c_ev, LV_STATE_CHECKED, c->ev_ready);
	lv_obj_set_state(c_evse, LV_STATE_CHECKED, c->evse_ready);
	set_text(c_plug_label, c->ev_plugged ? "Unplug" : "Plug in");
}

/* ---- Charging ------------------------------------------------------------- */

static void charging_build(lv_obj_t *v)
{
	static const char *const captions[4] = {"Power", "Current", "Voltage", "Duration"};
	lv_obj_t *r, *o, *both;

	r = row(v, px(8));
	lv_obj_set_pos(r, px(42), px(40));
	g_icon = label(r, LV_SYMBOL_CHARGE, lay.icon, C_GREEN);
	g_headline = label(r, "", lay.h2, C_GREEN);
	g_sub = label(v, "", lay.body, C_GREY);
	lv_obj_set_pos(g_sub, px(66), px(72));

	/* the energy of the session, as large as the screen allows */
	r = box(v, LV_FLEX_FLOW_ROW, 0, px(8));
	lv_obj_set_flex_align(r, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_align(r, LV_ALIGN_TOP_RIGHT, px(-42), px(4));
	g_hero = label(r, "0", lay.hero, C_INK);
	g_hero_unit = label(r, "Wh", lay.h1, C_GREY);
	lv_obj_set_style_pad_bottom(g_hero_unit, px(34), 0);

	socket_build(&g_socket, v, px(124));
	lv_obj_set_pos(g_socket.ring, px(48), px(122));

	r = row(v, px(30));
	lv_obj_align(r, LV_ALIGN_BOTTOM_LEFT, px(42), px(-48));
	for (int i = 0; i < 4; i++) {
		stat_build(&g_stat[i], r, captions[i]);
	}

	r = row(v, px(12));
	spill(r);
	lv_obj_align(r, LV_ALIGN_BOTTOM_RIGHT, px(-42), px(-44));
	both = box(r, LV_FLEX_FLOW_COLUMN, 0, px(3));
	spill(both);
	lv_obj_set_flex_align(both, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	round_button(both, px(42), LV_SYMBOL_PAUSE, ev_toggled, &g_ev_icon);
	label(both, "EV", lay.small, C_GREY);
	both = box(r, LV_FLEX_FLOW_COLUMN, 0, px(3));
	spill(both);
	lv_obj_set_flex_align(both, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	round_button(both, px(42), LV_SYMBOL_POWER, evse_toggled, &g_evse_icon);
	label(both, "EVSE", lay.small, C_GREY);
	o = pill(r, "Stop charging", C_RED, C_WHITE, stop_clicked, NULL);
	lv_obj_set_size(o, px(150), px(46));
	lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
	lv_obj_set_style_margin_bottom(o, px(16), 0);

	g_bar = bar(v, px(8));
	lv_obj_set_width(g_bar, px(716));
	lv_obj_align(g_bar, LV_ALIGN_BOTTOM_MID, 0, px(-14));
}

static void charging_refresh(const struct cp_connector *c)
{
	uint32_t accent = C_GREEN;
	const char *headline;
	char buf[64];
	char num[16];

	switch (c->status) {
	case EVSE_STATUS_SUSPENDED_EV:
		headline = "Paused by the vehicle";
		accent = C_AMBER;
		break;
	case EVSE_STATUS_SUSPENDED_EVSE:
		headline = "Paused by the station";
		accent = C_AMBER;
		break;
	default:
		fmt_kw(num, sizeof(num), c->power_w);
		snprintf(buf, sizeof(buf), "Charging at %s kW", num);
		headline = buf;
		break;
	}
	set_ink(g_icon, accent);
	set_ink(g_headline, accent);
	set_text(g_headline, headline);

	if (c->txn_id >= 0) {
		snprintf(buf, sizeof(buf), "Transaction %d  " LV_SYMBOL_BULLET "  %s", c->txn_id,
			 c->idtag);
	} else {
		snprintf(buf, sizeof(buf), "%s", c->idtag);
	}
	set_text(g_sub, buf);

	set_text(g_hero_unit, fmt_energy(num, sizeof(num), c->session_wh));
	set_text(g_hero, num);

	socket_set(&g_socket, accent, c->power_w > 0 ? C_MINT : C_INK);

	fmt_kw(num, sizeof(num), c->power_w);
	stat_set(&g_stat[0], num, "kW");
	snprintf(num, sizeof(num), "%u.%u", c->current_ma / 1000, (c->current_ma % 1000) / 100);
	stat_set(&g_stat[1], num, "A");
	snprintf(num, sizeof(num), "%u", c->voltage_v);
	stat_set(&g_stat[2], num, "V");
	fmt_duration(num, sizeof(num), c->session_s);
	stat_set(&g_stat[3], num, "");

	/* what the button does next: pause a ready side, resume a paused one */
	set_text(g_ev_icon, c->ev_ready ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
	set_ink(g_ev_icon, c->ev_ready ? C_INK : C_AMBER);
	set_ink(g_evse_icon, c->evse_ready ? C_GREEN : C_AMBER);

	lv_obj_set_style_bg_color(g_bar, col(accent), LV_PART_INDICATOR);
	lv_bar_set_value(g_bar,
			 c->max_power_w != 0 ? (int)(c->power_w * 1000ULL / c->max_power_w) : 0,
			 LV_ANIM_ON);
}

/* ---- Complete ------------------------------------------------------------- */

static void complete_build(lv_obj_t *v)
{
	static const char *const captions[4] = {"Energy delivered", "Duration", "Transaction",
						"Card"};
	lv_obj_t *r, *o, *c;

	r = row(v, px(12));
	lv_obj_align(r, LV_ALIGN_TOP_RIGHT, px(-42), px(64));
	label(r, "Charging complete", lay.h1, C_INK);
	label(r, LV_SYMBOL_CHARGE, lay.icon_l, C_GREEN);
	d_sub = label(v, "", lay.body, C_GREY);
	lv_obj_align(d_sub, LV_ALIGN_TOP_RIGHT, px(-42), px(108));

	r = row(v, px(12));
	spill(r);
	lv_obj_set_size(r, px(716), px(100));
	lv_obj_set_pos(r, px(42), px(160));
	for (int i = 0; i < 4; i++) {
		c = card(r, px(16), 0);
		lv_obj_set_flex_grow(c, 1);
		lv_obj_set_height(c, lv_pct(100));
		lv_obj_set_flex_align(c, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START,
				      LV_FLEX_ALIGN_START);
		stat_build(&d_stat[i], c, captions[i]);
	}

	c = box(v, LV_FLEX_FLOW_COLUMN, 0, px(2));
	lv_obj_align(c, LV_ALIGN_BOTTOM_LEFT, px(42), px(-34));
	d_meter = label(c, "", lay.h2, C_INK);
	label(c, "Meter reading sent with the StopTransaction", lay.body, C_GREY);

	o = pill(v, "Unplug to finish", C_INK, C_WHITE, unplug_clicked, &d_button_label);
	lv_obj_set_size(o, px(170), px(52));
	lv_obj_align(o, LV_ALIGN_BOTTOM_RIGHT, px(-42), px(-30));
}

static void complete_refresh(const struct cp_connector *c)
{
	const char *unit;
	char buf[64];
	char num[16];

	switch (c->stop_reason) {
	case EVSE_STOP_REMOTE:
		set_text(d_sub, "Stopped by the central system");
		break;
	case EVSE_STOP_EV_DISCONNECTED:
		set_text(d_sub, "The cable was pulled");
		break;
	default:
		set_text(d_sub, "Stopped at the station");
		break;
	}

	unit = fmt_energy(num, sizeof(num), c->session_wh);
	stat_set(&d_stat[0], num, unit);
	fmt_duration(num, sizeof(num), c->session_s);
	stat_set(&d_stat[1], num, "");
	if (c->last_txn_id >= 0) {
		snprintf(num, sizeof(num), "#%d", c->last_txn_id);
	} else {
		snprintf(num, sizeof(num), "-");
	}
	stat_set(&d_stat[2], num, "");
	stat_set(&d_stat[3], c->idtag, "");

	snprintf(buf, sizeof(buf), "%u Wh", c->energy_wh);
	set_text(d_meter, buf);
	set_text(d_button_label,
		 c->txn == EVSE_TXN_STOPPING ? "Stopping..." : "Unplug to finish");
}

/* ---- Out of order --------------------------------------------------------- */

static void fault_build(lv_obj_t *v)
{
	lv_obj_t *o;

	o = label(v, LV_SYMBOL_WARNING, lay.icon_l, C_RED);
	lv_obj_set_pos(o, px(42), px(84));
	f_headline = label(v, "", lay.h1, C_INK);
	lv_obj_set_pos(f_headline, px(42), px(124));
	f_sub = label(v, "", lay.body, C_GREY);
	lv_obj_set_pos(f_sub, px(42), px(168));

	o = pill(v, "Open diagnostics", C_INK, C_WHITE, diag_clicked, NULL);
	lv_obj_set_size(o, px(190), px(50));
	lv_obj_set_pos(o, px(42), px(214));
}

static void fault_refresh(const struct cp_connector *c)
{
	char buf[64];

	if (c->faulted) {
		set_text(f_headline, "Out of order");
		snprintf(buf, sizeof(buf), "Connector %d reports a hardware fault", sel);
	} else {
		set_text(f_headline, "Out of service");
		snprintf(buf, sizeof(buf), "Connector %d is not available for charging", sel);
	}
	set_text(f_sub, buf);
}

/* ---- Diagnostics ---------------------------------------------------------- */

/* "caption  value ........ (edit)": the value label is returned */
static lv_obj_t *field(lv_obj_t *parent, const char *caption, lv_event_cb_t cb)
{
	lv_obj_t *f = row(parent, px(8));
	lv_obj_t *l;

	lv_obj_set_width(f, lv_pct(100));
	lv_obj_set_style_pad_hor(f, px(12), 0);
	lv_obj_set_style_pad_ver(f, px(8), 0);
	surface(f, C_WHITE, px(10), false);
	lv_obj_set_style_border_width(f, 1, 0);
	lv_obj_set_style_border_color(f, col(C_TRACK), 0);
	on_click(f, cb, NULL);

	label(f, caption, lay.small, C_GREY);
	l = label(f, "", lay.body, C_INK);
	lv_obj_set_flex_grow(l, 1);
	lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
	lv_obj_set_height(l, lv_font_get_line_height(lay.body));
	label(f, LV_SYMBOL_EDIT, lay.small, C_SOFT);

	return l;
}

static void diag_build(lv_obj_t *v)
{
	static const char *const checks[CHECKS] = {
		"Backend URL set",
		"Network up",
		"WebSocket connected",
		"BootNotification accepted",
	};
	lv_obj_t *o, *c, *r, *tag;

	o = round_button(v, px(40), LV_SYMBOL_LEFT, diag_clicked, NULL);
	lv_obj_set_pos(o, px(36), px(14));
	lv_obj_set_style_shadow_width(o, 0, 0);
	lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
	o = label(v, "Diagnostics", lay.h1, C_INK);
	lv_obj_set_pos(o, px(86), px(6));
	x_sub = label(v, "", lay.body, C_GREY);
	lv_obj_set_pos(x_sub, px(86), px(46));

	/* identity on the right, with the endpoint as a code */
	c = card(v, px(8), 0);
	lv_obj_align(c, LV_ALIGN_TOP_RIGHT, px(-42), 0);
	x_qr = qr_code(c, px(64));
	tag = box(v, LV_FLEX_FLOW_COLUMN, 0, px(2));
	lv_obj_set_flex_align(tag, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_END);
	lv_obj_align(tag, LV_ALIGN_TOP_RIGHT, px(-136), px(18));
	on_click(tag, cbid_clicked, NULL);
	label(tag, "CHARGE BOX  " LV_SYMBOL_EDIT, lay.small, C_SOFT);
	x_cbid = label(tag, "", lay.brand, C_INK);

	/* link to the central system, step by step */
	c = card(v, px(18), px(9));
	lv_obj_set_size(c, px(360), px(306));
	lv_obj_set_pos(c, px(42), px(92));
	label(c, "CENTRAL SYSTEM", lay.small, C_SOFT);
	for (int i = 0; i < CHECKS; i++) {
		r = row(c, px(10));
		lv_obj_set_width(r, lv_pct(100));
		x_check_box[i] = box(r, LV_FLEX_FLOW_ROW, 0, 0);
		lv_obj_set_size(x_check_box[i], px(22), px(22));
		lv_obj_set_flex_align(x_check_box[i], LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
				      LV_FLEX_ALIGN_CENTER);
		surface(x_check_box[i], C_GREEN, px(7), false);
		lv_obj_set_style_border_color(x_check_box[i], col(C_TRACK), 0);
		x_check_icon[i] = label(x_check_box[i], LV_SYMBOL_OK, lay.small, C_WHITE);
		x_check_text[i] = label(r, checks[i], lay.body, C_INK);
		lv_obj_set_flex_grow(x_check_text[i], 1);
		x_check_state[i] = label(r, "", lay.small, C_GREEN);
	}
	x_url = field(c, "URL", url_clicked);
	lv_obj_set_style_margin_top(lv_obj_get_parent(x_url), px(4), 0);
	r = row(c, px(10));
	lv_obj_set_width(r, lv_pct(100));
	o = pill(r, "Restart", C_INK, C_WHITE, restart_clicked, NULL);
	lv_obj_set_size(o, px(104), px(38));
	x_hint = label(r, "", lay.small, C_AMBER);
	lv_obj_set_flex_grow(x_hint, 1);
	lv_label_set_long_mode(x_hint, LV_LABEL_LONG_MODE_WRAP);

	/* the connector on display */
	c = card(v, px(18), px(10));
	lv_obj_set_size(c, px(338), px(306));
	lv_obj_align(c, LV_ALIGN_TOP_RIGHT, px(-42), px(92));
	x_conn_title = label(c, "", lay.small, C_SOFT);

	r = row(c, px(6));
	lv_obj_set_width(r, lv_pct(100));
	o = label(r, "Power offered", lay.body, C_INK);
	lv_obj_set_flex_grow(o, 1);
	x_limit = label(r, "", lay.h2, C_INK);
	label(r, "kW", lay.small, C_GREY);
	x_limit_bar = bar(c, px(8));
	lv_obj_set_width(x_limit_bar, lv_pct(100));

	o = box(c, LV_FLEX_FLOW_ROW, 0, 0);
	lv_obj_set_height(o, px(2));
	x_fault = toggle_row(c, "Hardware fault", fault_toggled);
	x_service = toggle_row(c, "In service", service_toggled);

	o = box(c, LV_FLEX_FLOW_ROW, 0, 0);
	lv_obj_set_flex_grow(o, 1);
	x_idtag = field(c, "Card", idtag_clicked);
}

static void check_set(int i, bool ok, bool reached)
{
	lv_obj_set_style_bg_opa(x_check_box[i], ok ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(x_check_box[i], ok ? 0 : 2, 0);
	lv_obj_set_hidden(x_check_icon[i], !ok);
	set_ink(x_check_text[i], ok ? C_INK : C_GREY);
	set_ink(x_check_state[i], ok ? C_GREEN : C_AMBER);
	set_text(x_check_state[i], ok ? "OK" : reached ? "Pending" : "");
}

static void diag_refresh(const struct cp_status *st, const struct app_config *cfg,
			 const struct cp_connector *c)
{
	struct backend_url wanted;
	char path[sizeof(st->ws_path)];
	char buf[sizeof(x_qr_text)];
	bool changed;

	if (st->link == CP_LINK_ONLINE) {
		snprintf(buf, sizeof(buf), "OCPP 1.6  " LV_SYMBOL_BULLET "  heartbeat every %d s",
			 st->heartbeat_s);
	} else {
		snprintf(buf, sizeof(buf), "OCPP 1.6  " LV_SYMBOL_BULLET "  %s",
			 cp_link_str(st->link));
	}
	set_text(x_sub, buf);
	set_text(x_cbid, cfg->cbid[0] != '\0' ? cfg->cbid : "(none)");

	/* each step is pending while it is the one being worked on */
	check_set(0, st->link > CP_LINK_UNCONFIGURED, true);
	check_set(1, st->link > CP_LINK_NO_NETWORK, st->link >= CP_LINK_NO_NETWORK);
	check_set(2, st->link > CP_LINK_CONNECTING, st->link >= CP_LINK_RESOLVING);
	check_set(3, st->link == CP_LINK_ONLINE, st->link >= CP_LINK_BOOTING);

	set_text(x_url, cfg->url[0] != '\0' ? cfg->url : "(none)");

	/* what is saved differs from what is running */
	if (cfg->url[0] == '\0') {
		changed = st->link != CP_LINK_UNCONFIGURED;
	} else if (backend_url_parse(cfg->url, &wanted) < 0 ||
		   backend_url_ws_path(&wanted, cfg->cbid, path, sizeof(path)) < 0) {
		changed = false;
	} else {
		changed = st->link == CP_LINK_UNCONFIGURED || wanted.tls != st->url.tls ||
			  wanted.port != st->url.port || strcmp(wanted.host, st->url.host) != 0 ||
			  strcmp(path, st->ws_path) != 0;
	}
	set_text(x_hint, changed ? "Restart to use the new backend" : "");

	/* the endpoint in use, for a phone or a note */
	if (st->link == CP_LINK_UNCONFIGURED) {
		snprintf(buf, sizeof(buf), "%s", cfg->cbid);
	} else {
		snprintf(buf, sizeof(buf), "%s://%s:%u%s", st->url.tls ? "wss" : "ws", st->url.host,
			 st->url.port, st->ws_path);
	}
	if (strcmp(x_qr_text, buf) != 0) {
		strcpy(x_qr_text, buf);
		lv_qrcode_update(x_qr, buf, strlen(buf));
	}

	snprintf(buf, sizeof(buf), "CONNECTOR %d  " LV_SYMBOL_BULLET "  %s", sel,
		 evse_status_str(c->status));
	set_text(x_conn_title, buf);
	fmt_kw(buf, sizeof(buf), c->limit_w);
	set_text(x_limit, buf);
	lv_bar_set_value(x_limit_bar,
			 c->max_power_w != 0 ? (int)(c->limit_w * 1000ULL / c->max_power_w) : 0,
			 LV_ANIM_ON);
	lv_obj_set_state(x_fault, LV_STATE_CHECKED, c->faulted);
	lv_obj_set_state(x_service, LV_STATE_CHECKED, !c->inoperative);
	set_text(x_idtag, cfg->idtag[sel - 1]);
}

/* ---- screen --------------------------------------------------------------- */

static enum view view_for(const struct cp_connector *c)
{
	if (diag_open) {
		return VIEW_DIAG;
	}
	if (c->faulted) {
		return VIEW_FAULT;
	}
	if (c->txn == EVSE_TXN_ACTIVE) {
		return VIEW_CHARGING;
	}
	if (c->txn == EVSE_TXN_STOPPING || c->status == EVSE_STATUS_FINISHING) {
		return VIEW_COMPLETE;
	}
	if (c->status == EVSE_STATUS_UNAVAILABLE) {
		return VIEW_FAULT;
	}
	if (c->status == EVSE_STATUS_PREPARING) {
		return VIEW_CONNECTED;
	}

	return VIEW_READY;
}

void ui_init(void)
{
	static void (*const build[VIEW_COUNT])(lv_obj_t *) = {
		[VIEW_READY] = ready_build,       [VIEW_CONNECTED] = connected_build,
		[VIEW_CHARGING] = charging_build, [VIEW_COMPLETE] = complete_build,
		[VIEW_FAULT] = fault_build,       [VIEW_DIAG] = diag_build,
	};
	lv_obj_t *screen = lv_screen_active();
	lv_obj_t *stage;
	bool large;

	lay.w = lv_display_get_horizontal_resolution(NULL);
	lay.h = lv_display_get_vertical_resolution(NULL);
	large = lay.w >= 700;

	lay.brand = bold(16);
	lay.h1 = bold(31);
	lay.h2 = bold(22);
	lay.num = bold(36);
	lay.hero = bold(150);
	lay.water = bold(204);
	lay.body = large ? &lv_font_montserrat_14 : &lv_font_montserrat_10;
	lay.small = large ? &lv_font_montserrat_12 : &lv_font_montserrat_10;
	lay.icon = large ? &lv_font_montserrat_20 : &lv_font_montserrat_14;
	lay.icon_l = large ? &lv_font_montserrat_28 : &lv_font_montserrat_20;

	/* the three powers a connector can be told to offer: 16, 10 and 6 A of a
	 * 16 A connector, rounded to 100 W
	 */
	preset_w[0] = CONFIG_OCPP_SIM_APP_MAX_POWER_W;
	preset_w[1] = (CONFIG_OCPP_SIM_APP_MAX_POWER_W * 5 / 8 + 50) / 100 * 100;
	preset_w[2] = (CONFIG_OCPP_SIM_APP_MAX_POWER_W * 3 / 8 + 50) / 100 * 100;

	lv_obj_remove_style_all(screen);
	lv_obj_set_style_bg_color(screen, col(C_BG_TOP), 0);
	lv_obj_set_style_bg_grad_color(screen, col(C_BG_BOT), 0);
	lv_obj_set_style_bg_grad_dir(screen, LV_GRAD_DIR_VER, 0);
	lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
	lv_obj_set_scrollable(screen, false);
	lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);

	topbar_build(screen);

	/* the views lie on top of each other, one is shown */
	stage = lv_obj_create(screen);
	lv_obj_remove_style_all(stage);
	lv_obj_set_width(stage, lv_pct(100));
	lv_obj_set_flex_grow(stage, 1);
	lv_obj_set_scrollable(stage, false);

	for (int i = 0; i < VIEW_COUNT; i++) {
		views[i] = lv_obj_create(stage);
		lv_obj_remove_style_all(views[i]);
		lv_obj_set_size(views[i], lv_pct(100), lv_pct(100));
		lv_obj_set_scrollable(views[i], false);
		lv_obj_set_clickable(views[i], false);
		build[i](views[i]);
		lv_obj_set_hidden(views[i], true);
	}

	editor_build(screen);
}

void ui_refresh(void)
{
	struct app_config cfg;
	struct cp_status st;
	struct cp_connector c;
	enum view view;
	int request;

	request = (int)atomic_set(&requested_page, -1);
	if (request >= 0) {
		diag_open = request == UI_PAGE_DIAG;
	}
	request = (int)atomic_set(&requested_connector, 0);
	if (request >= 1 && request <= CP_CONNECTORS) {
		sel = request;
	}

	config_get(&cfg);
	cp_get_status(&st);
	cp_get_connector(sel, &c);

	view = view_for(&c);
	if (view != current_view) {
		current_view = view;
		for (int i = 0; i < VIEW_COUNT; i++) {
			lv_obj_set_hidden(views[i], i != view);
		}
	}

	topbar_refresh(&st, &cfg, &c);

	switch (view) {
	case VIEW_READY:
		ready_refresh(&c, cfg.idtag[sel - 1]);
		break;
	case VIEW_CONNECTED:
		connected_refresh(&c, cfg.idtag[sel - 1]);
		break;
	case VIEW_CHARGING:
		charging_refresh(&c);
		break;
	case VIEW_COMPLETE:
		complete_refresh(&c);
		break;
	case VIEW_FAULT:
		fault_refresh(&c);
		break;
	default:
		diag_refresh(&st, &cfg, &c);
		break;
	}
}

void ui_request_page(enum ui_page page)
{
	atomic_set(&requested_page, page);
}

void ui_request_connector(int connector)
{
	atomic_set(&requested_connector, connector);
}

int ui_page_from_name(const char *name)
{
	if (strcmp(name, "main") == 0) {
		return UI_PAGE_MAIN;
	}
	if (strcmp(name, "diag") == 0) {
		return UI_PAGE_DIAG;
	}

	return -EINVAL;
}
