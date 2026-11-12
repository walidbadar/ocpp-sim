/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 */

#include "config.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "url.h"

LOG_MODULE_REGISTER(config, LOG_LEVEL_INF);

static struct app_config cfg;
static K_MUTEX_DEFINE(lock);

static void copy(char *dst, size_t size, const char *src)
{
	strncpy(dst, src, size - 1);
	dst[size - 1] = '\0';
}

static int read_string(settings_read_cb read_cb, void *cb_arg, size_t len, char *dst, size_t size)
{
	ssize_t n;

	if (len >= size) {
		return -ENAMETOOLONG;
	}
	n = read_cb(cb_arg, dst, len);
	if (n < 0) {
		return (int)n;
	}
	dst[n] = '\0';

	return 0;
}

static int settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg)
{
	const char *next;

	if (settings_name_steq(name, "url", &next) && next == NULL) {
		return read_string(read_cb, cb_arg, len, cfg.url, sizeof(cfg.url));
	}
	if (settings_name_steq(name, "cbid", &next) && next == NULL) {
		return read_string(read_cb, cb_arg, len, cfg.cbid, sizeof(cfg.cbid));
	}
	if (settings_name_steq(name, "tag", &next) && next != NULL) {
		int connector = atoi(next);

		if (connector < 1 || connector > CONFIG_OCPP_SIM_APP_CONNECTORS) {
			return -ENOENT;
		}
		return read_string(read_cb, cb_arg, len, cfg.idtag[connector - 1],
				   sizeof(cfg.idtag[0]));
	}

	return -ENOENT;
}

SETTINGS_STATIC_HANDLER_DEFINE(sim, "sim", NULL, settings_set, NULL, NULL);

int config_init(void)
{
	int ret;

	copy(cfg.url, sizeof(cfg.url), CONFIG_OCPP_SIM_APP_URL);
	copy(cfg.cbid, sizeof(cfg.cbid), CONFIG_OCPP_SIM_APP_CHARGE_BOX_ID);
	for (int i = 0; i < CONFIG_OCPP_SIM_APP_CONNECTORS; i++) {
		snprintf(cfg.idtag[i], sizeof(cfg.idtag[i]), "%s%d",
			 CONFIG_OCPP_SIM_APP_ID_TAG_PREFIX, i + 1);
	}

	ret = settings_subsys_init();
	if (ret < 0) {
		LOG_ERR("settings: %d, running on the defaults", ret);
		return ret;
	}

	return settings_load_subtree("sim");
}

void config_get(struct app_config *out)
{
	k_mutex_lock(&lock, K_FOREVER);
	*out = cfg;
	k_mutex_unlock(&lock);
}

static int store(const char *key, char *dst, size_t size, const char *value)
{
	int ret;

	k_mutex_lock(&lock, K_FOREVER);
	copy(dst, size, value);
	k_mutex_unlock(&lock);

	ret = settings_save_one(key, value, strlen(value));
	if (ret < 0) {
		LOG_ERR("saving %s: %d", key, ret);
	}

	return ret;
}

int config_set_url(const char *url)
{
	struct backend_url parsed;
	int ret;

	if (strlen(url) > CONFIG_URL_MAX) {
		return -ENAMETOOLONG;
	}
	/* empty is allowed: the simulator then stays off line */
	if (url[0] != '\0') {
		ret = backend_url_parse(url, &parsed);
		if (ret < 0) {
			return ret;
		}
	}

	return store("sim/url", cfg.url, sizeof(cfg.url), url);
}

int config_set_cbid(const char *cbid)
{
	if (strlen(cbid) > CONFIG_CBID_MAX) {
		return -ENAMETOOLONG;
	}

	return store("sim/cbid", cfg.cbid, sizeof(cfg.cbid), cbid);
}

int config_set_idtag(int connector, const char *idtag)
{
	char key[16];
	size_t len = strlen(idtag);

	if (connector < 1 || connector > CONFIG_OCPP_SIM_APP_CONNECTORS || len == 0) {
		return -EINVAL;
	}
	if (len > EVSE_IDTAG_MAX) {
		return -ENAMETOOLONG;
	}

	snprintf(key, sizeof(key), "sim/tag/%d", connector);

	return store(key, cfg.idtag[connector - 1], sizeof(cfg.idtag[0]), idtag);
}

void config_override(const char *url, const char *cbid)
{
	k_mutex_lock(&lock, K_FOREVER);
	if (url != NULL) {
		copy(cfg.url, sizeof(cfg.url), url);
	}
	if (cbid != NULL) {
		copy(cfg.cbid, sizeof(cfg.cbid), cbid);
	}
	k_mutex_unlock(&lock);
}
