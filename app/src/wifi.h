/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 *
 * Boards with Wi-Fi: joins the network of the wifi_credentials library.
 */
#ifndef OCPP_SIM_APP_WIFI_H_
#define OCPP_SIM_APP_WIFI_H_

#if defined(CONFIG_WIFI)

/* Join the stored network, and again whenever the link drops */
void wifi_init(void);

#else

static inline void wifi_init(void)
{
}

#endif /* CONFIG_WIFI */

#endif /* OCPP_SIM_APP_WIFI_H_ */
