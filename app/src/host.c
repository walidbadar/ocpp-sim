/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 *
 * native_sim only.
 *
 *   --url=<ws://host:port/path>  backend for this run (not saved)
 *   --id=<charge box id>         identity for this run (not saved)
 *   --page=<name>                main or diag
 *   --shot=<file.ppm>            write the screen after --shot-delay=<ms>
 *                                (default 1500) and keep running
 */

#include "host.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include <zephyr/device.h>
#include <zephyr/input/input.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/clock.h>
#include <zephyr/sys/printk.h>

#include <cmdline.h>
#include <lvgl.h>
#include <native_rtc.h>
#include <posix_native_task.h>

#include "config.h"
#include "ui.h"

static char *opt_url;
static char *opt_id;
static char *opt_page;
static char *opt_shot;
static unsigned int opt_shot_delay_ms;

static char shot_path[256];
static bool shot_pending;
static bool opt_shot_done;
static K_MUTEX_DEFINE(shot_lock);

static void add_options(void)
{
	static struct args_struct_t options[] = {
		{.option = "url", .name = "ws://host:port/path", .type = 's',
		 .dest = (void *)&opt_url,
		 .descript = "Backend URL of the central system for this run"},
		{.option = "id", .name = "charge_box_id", .type = 's', .dest = (void *)&opt_id,
		 .descript = "Charge box identity for this run"},
		{.option = "page", .name = "name", .type = 's', .dest = (void *)&opt_page,
		 .descript = "Page to open (main, diag)"},
		{.option = "shot", .name = "file.ppm", .type = 's', .dest = (void *)&opt_shot,
		 .descript = "Write a screenshot (binary PPM) and keep running"},
		{.option = "shot-delay", .name = "ms", .type = 'u',
		 .dest = (void *)&opt_shot_delay_ms,
		 .descript = "Delay before the screenshot (default 1500 ms)"},
		ARG_TABLE_ENDMARKER,
	};

	native_add_command_line_opts(options);
}

NATIVE_TASK(add_options, PRE_BOOT_1, 10);

void host_init(void)
{
	uint64_t us = native_rtc_gettime_us(RTC_CLOCK_PSEUDOHOSTREALTIME);
	struct timespec ts = {
		.tv_sec = us / USEC_PER_SEC,
		.tv_nsec = (us % USEC_PER_SEC) * NSEC_PER_USEC,
	};
	int page;

	/* the OCPP timestamps are the host's time of day */
	sys_clock_settime(SYS_CLOCK_REALTIME, &ts);

	/* numeric options that were not given are left at UINT32_MAX */
	if (opt_shot_delay_ms == UINT32_MAX) {
		opt_shot_delay_ms = 1500;
	}

	config_override(opt_url, opt_id);

	if (opt_page != NULL) {
		page = ui_page_from_name(opt_page);
		if (page < 0) {
			printk("host: no page \"%s\"\n", opt_page);
		} else {
			ui_request_page(page);
		}
	}
}

static void screenshot(const char *path)
{
	lv_draw_buf_t *buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
	uint32_t w, h, stride;
	FILE *fp;

	if (buf == NULL) {
		printk("screenshot: snapshot failed\n");
		return;
	}
	fp = fopen(path, "wb");
	if (fp == NULL) {
		printk("screenshot: cannot write %s\n", path);
		lv_draw_buf_destroy(buf);
		return;
	}

	w = buf->header.w;
	h = buf->header.h;
	stride = buf->header.stride;
	fprintf(fp, "P6\n%u %u\n255\n", w, h);
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *row = (const uint8_t *)buf->data + y * stride;

		for (uint32_t x = 0; x < w; x++) {
			/* LVGL RGB888 is stored as B, G, R */
			uint8_t px[3] = {row[x * 3 + 2], row[x * 3 + 1], row[x * 3]};

			fwrite(px, 1, 3, fp);
		}
	}
	fclose(fp);
	lv_draw_buf_destroy(buf);
	printk("screenshot: wrote %s (%ux%u)\n", path, w, h);
}

int host_request_shot(const char *path)
{
	if (strlen(path) >= sizeof(shot_path)) {
		return -ENAMETOOLONG;
	}

	k_mutex_lock(&shot_lock, K_FOREVER);
	strcpy(shot_path, path);
	shot_pending = true;
	k_mutex_unlock(&shot_lock);

	return 0;
}

int host_tap(int x, int y)
{
	const struct device *touch = DEVICE_DT_GET(DT_CHOSEN(zephyr_touch));

	/* the events LVGL gets from the SDL window, without the window */
	input_report_abs(touch, INPUT_ABS_X, x, false, K_FOREVER);
	input_report_abs(touch, INPUT_ABS_Y, y, false, K_FOREVER);
	input_report_key(touch, INPUT_BTN_TOUCH, 1, true, K_FOREVER);
	k_msleep(80);
	input_report_key(touch, INPUT_BTN_TOUCH, 0, true, K_FOREVER);

	return 0;
}

void host_poll(int64_t now_ms)
{
	char path[sizeof(shot_path)];
	bool pending;

	if (opt_shot != NULL && !opt_shot_done && now_ms >= opt_shot_delay_ms) {
		opt_shot_done = true;
		screenshot(opt_shot);
	}

	k_mutex_lock(&shot_lock, K_FOREVER);
	pending = shot_pending;
	shot_pending = false;
	strcpy(path, shot_path);
	k_mutex_unlock(&shot_lock);

	if (pending) {
		screenshot(path);
	}
}
