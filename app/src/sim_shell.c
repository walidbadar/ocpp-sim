/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 *
 * "sim" on the console: everything the display can do, for scripts and
 * for boards on a serial port.
 */

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

#include "config.h"
#include "cp.h"
#include "host.h"
#include "ui.h"

static int parse_connector(const struct shell *sh, const char *arg)
{
	char *end;
	long connector = strtol(arg, &end, 10);

	if (*end != '\0' || connector < 1 || connector > CP_CONNECTORS) {
		shell_error(sh, "connector 1..%d", CP_CONNECTORS);
		return -EINVAL;
	}

	return (int)connector;
}

static int parse_onoff(const struct shell *sh, const char *arg, bool *on)
{
	int err = 0;

	*on = shell_strtobool(arg, 0, &err);
	if (err != 0) {
		shell_error(sh, "on or off");
		return -EINVAL;
	}

	return 0;
}

static void print_connector(const struct shell *sh, int connector)
{
	struct cp_connector c;

	cp_get_connector(connector, &c);
	shell_print(sh,
		    "connector %d: %s  plug=%d ev=%d evse=%d fault=%d  auth=%s tag=%s txn=%d  "
		    "%u W  %u Wh (session %u Wh)  %u.%u A  %u V  limit %u W",
		    connector, evse_status_str(c.status), c.ev_plugged, c.ev_ready, c.evse_ready,
		    c.faulted, evse_auth_str(c.auth), c.idtag[0] != '\0' ? c.idtag : "-",
		    c.txn_id, c.power_w, c.energy_wh, c.session_wh, c.current_ma / 1000,
		    (c.current_ma % 1000) / 100, c.voltage_v, c.limit_w);
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	struct cp_status st;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	cp_get_status(&st);
	if (st.link == CP_LINK_UNCONFIGURED) {
		shell_print(sh, "backend: none");
	} else {
		shell_print(sh, "backend: %s://%s:%u%s  %s", st.url.tls ? "wss" : "ws",
			    st.url.host, st.url.port, st.ws_path, cp_link_str(st.link));
	}
	for (int connector = 1; connector <= CP_CONNECTORS; connector++) {
		print_connector(sh, connector);
	}

	return 0;
}

static int cmd_plug(const struct shell *sh, size_t argc, char **argv)
{
	int connector = parse_connector(sh, argv[1]);

	ARG_UNUSED(argc);

	if (connector < 0) {
		return connector;
	}
	cp_set_plugged(connector, strcmp(argv[0], "plug") == 0);

	return 0;
}

static int cmd_input(const struct shell *sh, size_t argc, char **argv)
{
	int connector = parse_connector(sh, argv[1]);
	bool on;

	ARG_UNUSED(argc);

	if (connector < 0 || parse_onoff(sh, argv[2], &on) < 0) {
		return -EINVAL;
	}

	if (strcmp(argv[0], "ev") == 0) {
		cp_set_ev_ready(connector, on);
	} else if (strcmp(argv[0], "evse") == 0) {
		cp_set_evse_ready(connector, on);
	} else if (strcmp(argv[0], "fault") == 0) {
		cp_set_fault(connector, on);
	} else {
		/* service on = in service */
		cp_set_inoperative(connector, !on);
	}

	return 0;
}

static int cmd_tag(const struct shell *sh, size_t argc, char **argv)
{
	int connector = parse_connector(sh, argv[1]);
	int ret;

	if (connector < 0) {
		return connector;
	}

	ret = cp_present_tag(connector, argc > 2 ? argv[2] : NULL);
	if (ret < 0) {
		shell_error(sh, "not accepted (%d)", ret);
	}

	return ret;
}

static int cmd_stop(const struct shell *sh, size_t argc, char **argv)
{
	int connector = parse_connector(sh, argv[1]);
	int ret;

	ARG_UNUSED(argc);

	if (connector < 0) {
		return connector;
	}

	ret = cp_stop(connector);
	if (ret < 0) {
		shell_error(sh, "nothing to stop");
	}

	return ret;
}

static int cmd_limit(const struct shell *sh, size_t argc, char **argv)
{
	int connector = parse_connector(sh, argv[1]);
	char *end;
	unsigned long watts = strtoul(argv[2], &end, 10);

	ARG_UNUSED(argc);

	if (connector < 0 || *end != '\0') {
		shell_error(sh, "limit <connector> <watts>");
		return -EINVAL;
	}
	cp_set_limit(connector, (uint32_t)watts);

	return 0;
}

static void saved(const struct shell *sh, int ret, bool restart)
{
	if (ret < 0) {
		shell_error(sh, "not saved (%d)", ret);
	} else if (restart) {
		shell_print(sh, "saved, takes effect after \"sim restart\"");
	} else {
		shell_print(sh, "saved");
	}
}

static int cmd_url(const struct shell *sh, size_t argc, char **argv)
{
	struct app_config cfg;
	int ret;

	if (argc < 2) {
		config_get(&cfg);
		shell_print(sh, "%s", cfg.url[0] != '\0' ? cfg.url : "(none)");
		return 0;
	}

	ret = config_set_url(strcmp(argv[1], "none") == 0 ? "" : argv[1]);
	saved(sh, ret, true);

	return ret;
}

static int cmd_id(const struct shell *sh, size_t argc, char **argv)
{
	struct app_config cfg;
	int ret;

	if (argc < 2) {
		config_get(&cfg);
		shell_print(sh, "%s", cfg.cbid);
		return 0;
	}

	ret = config_set_cbid(argv[1]);
	saved(sh, ret, true);

	return ret;
}

static int cmd_idtag(const struct shell *sh, size_t argc, char **argv)
{
	int connector = parse_connector(sh, argv[1]);
	struct app_config cfg;
	int ret;

	if (connector < 0) {
		return connector;
	}
	if (argc < 3) {
		config_get(&cfg);
		shell_print(sh, "%s", cfg.idtag[connector - 1]);
		return 0;
	}

	ret = config_set_idtag(connector, argv[2]);
	saved(sh, ret, false);

	return ret;
}

static int cmd_page(const struct shell *sh, size_t argc, char **argv)
{
	int page = ui_page_from_name(argv[1]);

	ARG_UNUSED(argc);

	if (page < 0) {
		shell_error(sh, "main or diag");
		return page;
	}
	ui_request_page(page);

	return 0;
}

static int cmd_view(const struct shell *sh, size_t argc, char **argv)
{
	int connector = parse_connector(sh, argv[1]);

	ARG_UNUSED(argc);

	if (connector < 0) {
		return connector;
	}
	ui_request_connector(connector);

	return 0;
}

static int cmd_shot(const struct shell *sh, size_t argc, char **argv)
{
	int ret = host_request_shot(argv[1]);

	ARG_UNUSED(argc);

	if (ret < 0) {
		shell_error(sh, "screenshots are a native_sim feature (%d)", ret);
	}

	return ret;
}

static int cmd_tap(const struct shell *sh, size_t argc, char **argv)
{
	int ret = host_tap(atoi(argv[1]), atoi(argv[2]));

	ARG_UNUSED(argc);

	if (ret < 0) {
		shell_error(sh, "taps are a native_sim feature (%d)", ret);
	}

	return ret;
}

static int cmd_restart(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "restarting");
	k_msleep(100);
	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sim_cmds,
	SHELL_CMD_ARG(status, NULL, "Backend and connectors", cmd_status, 1, 0),
	SHELL_CMD_ARG(plug, NULL, "<connector>: plug the EV in", cmd_plug, 2, 0),
	SHELL_CMD_ARG(unplug, NULL, "<connector>: pull the cable", cmd_plug, 2, 0),
	SHELL_CMD_ARG(ev, NULL, "<connector> <on|off>: the EV asks for energy", cmd_input, 3, 0),
	SHELL_CMD_ARG(evse, NULL, "<connector> <on|off>: the EVSE offers energy", cmd_input, 3, 0),
	SHELL_CMD_ARG(tag, NULL,
		      "<connector> [idTag]: present an identifier (the saved one if omitted)",
		      cmd_tag, 2, 1),
	SHELL_CMD_ARG(stop, NULL, "<connector>: stop button", cmd_stop, 2, 0),
	SHELL_CMD_ARG(fault, NULL, "<connector> <on|off>: hardware fault", cmd_input, 3, 0),
	SHELL_CMD_ARG(service, NULL, "<connector> <on|off>: in or out of service", cmd_input, 3,
		      0),
	SHELL_CMD_ARG(limit, NULL, "<connector> <watts>: power the EVSE offers", cmd_limit, 3, 0),
	SHELL_CMD_ARG(url, NULL, "[ws://host:port/path | none]: backend URL", cmd_url, 1, 1),
	SHELL_CMD_ARG(id, NULL, "[identity]: charge box identity", cmd_id, 1, 1),
	SHELL_CMD_ARG(idtag, NULL, "<connector> [idTag]: identifier of the connector", cmd_idtag,
		      2, 1),
	SHELL_CMD_ARG(page, NULL, "<main|diag>: page on the display", cmd_page, 2, 0),
	SHELL_CMD_ARG(view, NULL, "<connector>: connector on the display", cmd_view, 2, 0),
	SHELL_CMD_ARG(shot, NULL, "<file.ppm>: screenshot (native_sim)", cmd_shot, 2, 0),
	SHELL_CMD_ARG(tap, NULL, "<x> <y>: touch the display (native_sim)", cmd_tap, 3, 0),
	SHELL_CMD_ARG(restart, NULL, "Restart with the saved backend", cmd_restart, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(sim, &sim_cmds, "Charge point simulator", NULL);
