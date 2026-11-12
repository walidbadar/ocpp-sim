/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 *
 * The LVGL user interface. Everything here runs on the main thread except
 * the ui_request_*() functions, which any thread may call.
 */
#ifndef OCPP_SIM_APP_UI_H_
#define OCPP_SIM_APP_UI_H_

enum ui_page {
	UI_PAGE_MAIN, /* what the selected connector is doing */
	UI_PAGE_DIAG, /* backend, identity, connector switches */

	UI_PAGE_COUNT
};

/* Build the screen for the resolution of the display */
void ui_init(void);

/* Bring the widgets up to date; call a few times a second */
void ui_refresh(void);

/* Show a page, or another connector, at the next refresh */
void ui_request_page(enum ui_page page);
void ui_request_connector(int connector);

/* Page for "main" or "diag"; -EINVAL otherwise */
int ui_page_from_name(const char *name);

#endif /* OCPP_SIM_APP_UI_H_ */
