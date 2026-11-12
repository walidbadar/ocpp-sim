/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file
 * @brief Backend URL of an OCPP-J central system: ws://host[:port][/path].
 */

#ifndef BACKEND_URL_H_
#define BACKEND_URL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Sizes include the terminating NUL. */
#define BACKEND_URL_HOST_MAX 64
#define BACKEND_URL_PATH_MAX 128

struct backend_url {
	bool tls;                      /**< wss:// */
	uint16_t port;                 /**< 80 or 443 unless given */
	char host[BACKEND_URL_HOST_MAX]; /**< name or address, IPv6 without brackets */
	char path[BACKEND_URL_PATH_MAX]; /**< starts with '/', no trailing '/' */
};

/**
 * @brief Split a backend URL.
 *
 * Accepts "ws://host", "wss://host:port/path" and, without scheme,
 * "host:port/path" (taken as ws). IPv6 addresses go in brackets.
 *
 * @retval 0 on success
 * @retval -EINVAL malformed (empty host, bad port, blanks, query or fragment)
 * @retval -EPROTONOSUPPORT a scheme other than ws or wss
 * @retval -ENAMETOOLONG host or path does not fit
 */
int backend_url_parse(const char *url, struct backend_url *out);

/**
 * @brief Path of the WebSocket endpoint of one charge point.
 *
 * OCPP-J appends the charge box identity to the backend path; characters
 * outside the URL unreserved set are percent-encoded. An empty or NULL
 * @p charge_box_id leaves the path as it is.
 *
 * @return length written (without NUL), or -ENAMETOOLONG
 */
int backend_url_ws_path(const struct backend_url *url, const char *charge_box_id, char *buf,
		      size_t len);

#ifdef __cplusplus
}
#endif

#endif /* BACKEND_URL_H_ */
