/*
 * Copyright (c) 2026 Muhammad Waleed Badar
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "url.h"

static struct backend_url u;
static char path[64];

ZTEST(backend_url, test_full_url)
{
	zassert_ok(backend_url_parse("ws://csms.example.com:8180/steve/websocket", &u));
	zassert_false(u.tls);
	zassert_str_equal(u.host, "csms.example.com");
	zassert_equal(u.port, 8180);
	zassert_str_equal(u.path, "/steve/websocket");
}

ZTEST(backend_url, test_defaults)
{
	zassert_ok(backend_url_parse("ws://10.0.0.5", &u));
	zassert_str_equal(u.host, "10.0.0.5");
	zassert_equal(u.port, 80);
	zassert_str_equal(u.path, "/");

	zassert_ok(backend_url_parse("wss://csms.example.com/ocpp/", &u));
	zassert_true(u.tls);
	zassert_equal(u.port, 443);
	zassert_str_equal(u.path, "/ocpp");
}

ZTEST(backend_url, test_without_scheme)
{
	zassert_ok(backend_url_parse("192.168.1.10:9000/ocpp", &u));
	zassert_false(u.tls);
	zassert_str_equal(u.host, "192.168.1.10");
	zassert_equal(u.port, 9000);
	zassert_str_equal(u.path, "/ocpp");
}

ZTEST(backend_url, test_ipv6)
{
	zassert_ok(backend_url_parse("ws://[fe80::1]:9000/ocpp", &u));
	zassert_str_equal(u.host, "fe80::1");
	zassert_equal(u.port, 9000);

	zassert_ok(backend_url_parse("ws://[::1]", &u));
	zassert_str_equal(u.host, "::1");
	zassert_equal(u.port, 80);

	zassert_equal(backend_url_parse("ws://[::1", &u), -EINVAL);
	zassert_equal(backend_url_parse("ws://[::1]x", &u), -EINVAL);
}

ZTEST(backend_url, test_malformed)
{
	zassert_equal(backend_url_parse(NULL, &u), -EINVAL);
	zassert_equal(backend_url_parse("", &u), -EINVAL);
	zassert_equal(backend_url_parse("ws://", &u), -EINVAL);
	zassert_equal(backend_url_parse("ws://:9000/x", &u), -EINVAL);
	zassert_equal(backend_url_parse("ws://host:/x", &u), -EINVAL);
	zassert_equal(backend_url_parse("ws://host:0/x", &u), -EINVAL);
	zassert_equal(backend_url_parse("ws://host:65536/x", &u), -EINVAL);
	zassert_equal(backend_url_parse("ws://host:90a0/x", &u), -EINVAL);
	zassert_equal(backend_url_parse("ws://host:123456/x", &u), -EINVAL);
	zassert_equal(backend_url_parse("ws://ho st/x", &u), -EINVAL);
	zassert_equal(backend_url_parse("ws://host/x?y=1", &u), -EINVAL);
	zassert_equal(backend_url_parse("ws://host/x#y", &u), -EINVAL);
	zassert_equal(backend_url_parse("http://host/x", &u), -EPROTONOSUPPORT);
}

ZTEST(backend_url, test_too_long)
{
	char url[BACKEND_URL_HOST_MAX + BACKEND_URL_PATH_MAX + 16] = "ws://";

	memset(url + 5, 'h', BACKEND_URL_HOST_MAX);
	zassert_equal(backend_url_parse(url, &u), -ENAMETOOLONG);

	/* one less fits */
	url[5 + BACKEND_URL_HOST_MAX - 1] = '\0';
	zassert_ok(backend_url_parse(url, &u));
	zassert_equal(strlen(u.host), BACKEND_URL_HOST_MAX - 1);

	strcpy(url, "ws://h/");
	memset(url + 7, 'p', BACKEND_URL_PATH_MAX);
	url[7 + BACKEND_URL_PATH_MAX] = '\0';
	zassert_equal(backend_url_parse(url, &u), -ENAMETOOLONG);
}

ZTEST(backend_url, test_ws_path)
{
	zassert_ok(backend_url_parse("ws://host:9000/ocpp", &u));
	zassert_equal(backend_url_ws_path(&u, "CP-01", path, sizeof(path)), 11);
	zassert_str_equal(path, "/ocpp/CP-01");

	/* no identity: the path as given */
	zassert_equal(backend_url_ws_path(&u, "", path, sizeof(path)), 5);
	zassert_str_equal(path, "/ocpp");
	zassert_equal(backend_url_ws_path(&u, NULL, path, sizeof(path)), 5);

	/* root path */
	zassert_ok(backend_url_parse("ws://host:9000", &u));
	zassert_true(backend_url_ws_path(&u, "CP-01", path, sizeof(path)) > 0);
	zassert_str_equal(path, "/CP-01");
}

ZTEST(backend_url, test_ws_path_encodes_the_identity)
{
	zassert_ok(backend_url_parse("ws://host/ocpp", &u));
	zassert_true(backend_url_ws_path(&u, "a b/c_d~e.f", path, sizeof(path)) > 0);
	zassert_str_equal(path, "/ocpp/a%20b%2Fc_d~e.f");
}

ZTEST(backend_url, test_ws_path_does_not_overflow)
{
	char small[12];

	zassert_ok(backend_url_parse("ws://host/ocpp", &u));
	/* "/ocpp/CP-01" needs 12 bytes with the NUL */
	zassert_equal(backend_url_ws_path(&u, "CP-01", small, sizeof(small)), 11);
	zassert_equal(backend_url_ws_path(&u, "CP-012", small, sizeof(small)), -ENAMETOOLONG);
	zassert_equal(backend_url_ws_path(&u, "CP 1", small, sizeof(small)), -ENAMETOOLONG);
	zassert_equal(backend_url_ws_path(&u, "", small, 5), -ENAMETOOLONG);
	zassert_equal(backend_url_ws_path(&u, "", small, 6), 5);
}

ZTEST_SUITE(backend_url, NULL, NULL, NULL, NULL, NULL);
