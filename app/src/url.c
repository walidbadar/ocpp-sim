/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 */

#include "url.h"

#include <ctype.h>
#include <errno.h>
#include <string.h>

static bool has_prefix(const char *s, const char *prefix)
{
	return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool is_unreserved(unsigned char c)
{
	return isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~';
}

int backend_url_parse(const char *url, struct backend_url *out)
{
	const char *p = url;
	const char *end;
	size_t len;

	if (url == NULL || out == NULL) {
		return -EINVAL;
	}

	for (const char *c = url; *c != '\0'; c++) {
		if (isspace((unsigned char)*c) || iscntrl((unsigned char)*c) || *c == '?' ||
		    *c == '#') {
			return -EINVAL;
		}
	}

	memset(out, 0, sizeof(*out));

	if (has_prefix(p, "ws://")) {
		p += 5;
	} else if (has_prefix(p, "wss://")) {
		out->tls = true;
		p += 6;
	} else if (strstr(p, "://") != NULL) {
		return -EPROTONOSUPPORT;
	}
	out->port = out->tls ? 443 : 80;

	/* host */
	if (*p == '[') {
		end = strchr(p, ']');
		if (end == NULL) {
			return -EINVAL;
		}
		p++;
		len = end - p;
		end++;
		if (*end != '\0' && *end != ':' && *end != '/') {
			return -EINVAL;
		}
	} else {
		end = p + strcspn(p, ":/");
		len = end - p;
	}
	if (len == 0) {
		return -EINVAL;
	}
	if (len >= sizeof(out->host)) {
		return -ENAMETOOLONG;
	}
	memcpy(out->host, p, len);
	p = end;

	/* port */
	if (*p == ':') {
		uint32_t port = 0;
		int digits = 0;

		for (p++; isdigit((unsigned char)*p); p++) {
			port = port * 10 + (*p - '0');
			if (++digits > 5) {
				return -EINVAL;
			}
		}
		if (digits == 0 || port == 0 || port > UINT16_MAX || (*p != '\0' && *p != '/')) {
			return -EINVAL;
		}
		out->port = (uint16_t)port;
	}

	/* path, without the trailing slashes */
	len = strlen(p);
	while (len > 0 && p[len - 1] == '/') {
		len--;
	}
	if (len == 0) {
		strcpy(out->path, "/");
		return 0;
	}
	if (len >= sizeof(out->path)) {
		return -ENAMETOOLONG;
	}
	memcpy(out->path, p, len);

	return 0;
}

int backend_url_ws_path(const struct backend_url *url, const char *charge_box_id, char *buf,
		      size_t len)
{
	static const char hex[] = "0123456789ABCDEF";
	size_t n = strlen(url->path);

	if (n + 1 > len) {
		return -ENAMETOOLONG;
	}
	memcpy(buf, url->path, n + 1);

	if (charge_box_id == NULL || charge_box_id[0] == '\0') {
		return (int)n;
	}

	/* "/" + id, but "/id" rather than "//id" for the root path */
	if (n == 1 && buf[0] == '/') {
		n = 0;
	}
	if (n + 2 > len) {
		return -ENAMETOOLONG;
	}
	buf[n++] = '/';

	for (const char *c = charge_box_id; *c != '\0'; c++) {
		unsigned char ch = (unsigned char)*c;

		if (is_unreserved(ch)) {
			if (n + 2 > len) {
				return -ENAMETOOLONG;
			}
			buf[n++] = (char)ch;
		} else {
			if (n + 4 > len) {
				return -ENAMETOOLONG;
			}
			buf[n++] = '%';
			buf[n++] = hex[ch >> 4];
			buf[n++] = hex[ch & 0x0f];
		}
	}
	buf[n] = '\0';

	return (int)n;
}
