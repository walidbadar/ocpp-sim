/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 *
 * What the simulator remembers across restarts: the backend URL, the
 * charge box identity and the identifier presented at each connector.
 */
#ifndef OCPP_SIM_APP_CONFIG_H_
#define OCPP_SIM_APP_CONFIG_H_

#include "evse.h"

#define CONFIG_URL_MAX  192
#define CONFIG_CBID_MAX 48

struct app_config {
	char url[CONFIG_URL_MAX + 1];
	char cbid[CONFIG_CBID_MAX + 1];
	char idtag[CONFIG_OCPP_SIM_APP_CONNECTORS][EVSE_IDTAG_MAX + 1];
};

/** Load the saved values over the Kconfig defaults. */
int config_init(void);

/** Copy of the current values. Any thread. */
void config_get(struct app_config *out);

/**
 * Change and save a value. The backend URL and the identity take effect at
 * the next start, the identifier with the next tap.
 *
 * @retval 0 saved
 * @retval -EINVAL, -EPROTONOSUPPORT, -ENAMETOOLONG not a usable value
 */
int config_set_url(const char *url);
int config_set_cbid(const char *cbid);
int config_set_idtag(int connector, const char *idtag);

/** Use these for this run without saving them (command line). */
void config_override(const char *url, const char *cbid);

#endif /* OCPP_SIM_APP_CONFIG_H_ */
