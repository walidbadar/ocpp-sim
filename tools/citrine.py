#!/usr/bin/env python3
# Copyright (c) 2026 Muhammad Waleed Badar
# SPDX-License-Identifier: Apache-2.0
"""Operator's side of the bench: a few things to do in a local CitrineOS.

    citrine.py setup                 station, its connectors and their identifiers
    citrine.py tag ABC123            accept an identifier (--status to refuse it)
    citrine.py tags | stations | transactions
    citrine.py start 1 ZEPHYR-1      RemoteStartTransaction on connector 1
    citrine.py stop                  RemoteStopTransaction of the running transaction
    citrine.py unlock 1              UnlockConnector
    citrine.py getconf [key]         GetConfiguration
    citrine.py setconf key value     ChangeConfiguration

The requests to the charge point go through the REST API of CitrineOS. The
records it needs first (a station with connectors, identifiers) are written
straight into its database with psql in the database container: CitrineOS
would learn the connectors from StatusNotification, which Zephyr's OCPP
library does not send, and it manages identifiers through its operator UI.
This is for the stack of tools/citrineos/compose.yml, not for a deployment.

Only the Python standard library is used.
"""

import argparse
import json
import shutil
import subprocess
import sys
import urllib.error
import urllib.parse
import urllib.request

DB_CONTAINER = "ocpp-sim-citrineos-ocpp-db-1"
STATION = "zephyr-sim-01"
TAG_STATUS = ("Accepted", "Blocked", "Expired", "Invalid", "ConcurrentTx")


def sql_str(value):
    """Quote a text literal for SQL."""
    return "'" + str(value).replace("'", "''") + "'"


class Bench:
    def __init__(self, args):
        self.api = args.api.rstrip("/")
        self.tenant = args.tenant
        self.station = args.station
        self.container = args.db_container
        self.runtime = args.runtime or self._find_runtime()

    def _find_runtime(self):
        """The container tool that runs the database: podman or docker."""
        for tool in ("podman", "docker"):
            if shutil.which(tool) is None:
                continue
            found = subprocess.run(
                [tool, "inspect", "--format", "{{.State.Running}}", self.container],
                capture_output=True,
                text=True,
                check=False,
            )
            if found.returncode == 0 and found.stdout.strip() == "true":
                return tool
        sys.exit(
            f"database container {self.container} is not running: start the stack with\n"
            "  podman compose -f tools/citrineos/compose.yml up -d   (or docker compose)"
        )

    def sql(self, statement):
        """Run a statement, return the rows as lists of strings."""
        done = subprocess.run(
            [self.runtime, "exec", "-i", self.container, "psql", "-U", "citrine", "-d", "citrine",
             "-v", "ON_ERROR_STOP=1", "-At", "-F", "\t", "-c", statement],
            capture_output=True,
            text=True,
            check=False,
        )
        if done.returncode != 0:
            sys.exit(f"psql: {done.stderr.strip()}")
        rows = [line.split("\t") for line in done.stdout.splitlines() if line]
        # "INSERT 0 1" and friends are command tags, not rows
        return [r for r in rows if not (len(r) == 1 and r[0].split(" ")[0] in
                                        ("INSERT", "UPDATE", "DELETE"))]

    def call(self, module, action, payload):
        """Send an OCPP 1.6 request to the station through the REST API."""
        query = urllib.parse.urlencode({"identifier": self.station, "tenantId": self.tenant})
        url = f"{self.api}/ocpp/1.6/{module}/{action}?{query}"
        request = urllib.request.Request(
            url,
            data=json.dumps(payload).encode(),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                body = response.read().decode()
        except urllib.error.HTTPError as err:
            sys.exit(f"{url}: HTTP {err.code} {err.read().decode()}")
        except urllib.error.URLError as err:
            sys.exit(f"{url}: {err.reason} (is CitrineOS running?)")
        try:
            return json.loads(body)
        except json.JSONDecodeError:
            return body

    # -- records ---------------------------------------------------------------

    def station_id(self):
        rows = self.sql(
            'select id from "ChargingStations" where "ocppConnectionName" = '
            f'{sql_str(self.station)} and "tenantId" = {self.tenant}'
        )
        return int(rows[0][0]) if rows else None

    def add_station(self):
        if self.station_id() is None:
            self.sql(
                'insert into "ChargingStations" ("ocppConnectionName", "tenantId", "createdAt", '
                f'"updatedAt") values ({sql_str(self.station)}, {self.tenant}, now(), now())'
            )
        return self.station_id()

    def add_connectors(self, count):
        """One EVSE per connector, the way CitrineOS commissions OCPP 1.6 stations."""
        station = self.add_station()
        have = {
            int(r[0])
            for r in self.sql(
                f'select "connectorId" from "Connectors" where "stationId" = {station} '
                f'and "tenantId" = {self.tenant}'
            )
        }
        added = []
        for connector in range(1, count + 1):
            if connector in have:
                continue
            self.sql(
                'with e as (insert into "Evses" ("tenantId", "stationId", "createdAt", '
                f'"updatedAt") values ({self.tenant}, {station}, now(), now()) returning id) '
                'insert into "Connectors" ("tenantId", "stationId", "evseId", "connectorId", '
                f'status, "createdAt", "updatedAt") select {self.tenant}, {station}, e.id, '
                f"{connector}, 'Available', now(), now() from e"
            )
            added.append(connector)
        return added

    def add_tag(self, idtag, status="Accepted"):
        if len(idtag) > 20:
            sys.exit(f"{idtag}: an OCPP 1.6 identifier has at most 20 characters")
        found = self.sql(
            f'select id from "Authorizations" where "idToken" = {sql_str(idtag)} '
            f'and "tenantId" = {self.tenant}'
        )
        # concurrentTransaction: a simulator that is closed while charging leaves its
        # transaction open here, and CitrineOS would refuse the card from then on
        if found:
            self.sql(
                f'update "Authorizations" set status = {sql_str(status)}, '
                f'"concurrentTransaction" = true, "updatedAt" = now() where id = {found[0][0]}'
            )
        else:
            self.sql(
                'insert into "Authorizations" ("idToken", "idTokenType", status, '
                '"concurrentTransaction", "tenantId", "createdAt", "updatedAt") values '
                f"({sql_str(idtag)}, 'ISO14443', {sql_str(status)}, true, {self.tenant}, "
                "now(), now())"
            )

    def active_transaction(self):
        rows = self.sql(
            'select t."transactionId" from "Transactions" t join "ChargingStations" s '
            'on s.id = t."stationId" where t."isActive" and s."ocppConnectionName" = '
            f'{sql_str(self.station)} and t."tenantId" = {self.tenant} order by t.id desc limit 1'
        )
        return rows[0][0] if rows else None


def table(header, rows):
    if not rows:
        print("(none)")
        return
    widths = [max(len(str(x)) for x in col) for col in zip(header, *rows)]
    for row in [header] + rows:
        print("  ".join(str(x).ljust(w) for x, w in zip(row, widths)).rstrip())


def cmd_setup(bench, args):
    added = bench.add_connectors(args.connectors)
    tags = args.tags or [f"{args.tag_prefix}{n}" for n in range(1, args.connectors + 1)]
    for tag in tags:
        bench.add_tag(tag)
    print(f"station {bench.station}: {args.connectors} connectors"
          + (f" (added {', '.join(map(str, added))})" if added else " (already there)"))
    print(f"identifiers accepted: {', '.join(tags)}")


def cmd_tag(bench, args):
    bench.add_tag(args.idtag, args.status)
    print(f"{args.idtag}: {args.status}")


def cmd_tags(bench, args):
    table(["idTag", "status"], bench.sql(
        f'select "idToken", status from "Authorizations" where "tenantId" = {bench.tenant} '
        "order by id"))


def cmd_stations(bench, args):
    table(["station", "online", "protocol", "vendor", "model", "firmware", "connectors"],
          bench.sql(
              'select s."ocppConnectionName", s."isOnline", coalesce(s.protocol, \'\'), '
              'coalesce(s."chargePointVendor", \'\'), coalesce(s."chargePointModel", \'\'), '
              'coalesce(s."firmwareVersion", \'\'), count(c.id) from "ChargingStations" s '
              'left join "Connectors" c on c."stationId" = s.id '
              f'where s."tenantId" = {bench.tenant} group by s.id order by s.id'))


def cmd_transactions(bench, args):
    table(["transaction", "station", "connector", "active", "meterStart", "kWh", "meter values",
           "stopped", "start"],
          bench.sql(
              'select t."transactionId", s."ocppConnectionName", coalesce(c."connectorId", 0), '
              't."isActive", coalesce(t."meterStart", 0), coalesce(t."totalKwh", 0), '
              '(select count(*) from "MeterValues" m where m."transactionDatabaseId" = t.id), '
              'coalesce(t."stoppedReason", \'\'), to_char(t."startTime", \'HH24:MI:SS\') '
              'from "Transactions" t join "ChargingStations" s on s.id = t."stationId" '
              'left join "Connectors" c on c.id = t."connectorId" '
              f'where t."tenantId" = {bench.tenant} order by t.id desc limit {int(args.limit)}'))


def show(result):
    print(json.dumps(result, indent=2) if not isinstance(result, str) else result)


def cmd_start(bench, args):
    show(bench.call("evdriver", "remoteStartTransaction",
                    {"connectorId": args.connector, "idTag": args.idtag}))


def cmd_stop(bench, args):
    transaction = args.transaction or bench.active_transaction()
    if transaction is None:
        sys.exit(f"no active transaction on {bench.station}")
    show(bench.call("evdriver", "remoteStopTransaction", {"transactionId": int(transaction)}))


def cmd_unlock(bench, args):
    show(bench.call("evdriver", "unlockConnector", {"connectorId": args.connector}))


def cmd_getconf(bench, args):
    show(bench.call("configuration", "getConfiguration", {"key": args.key} if args.key else {}))


def cmd_setconf(bench, args):
    show(bench.call("configuration", "changeConfiguration",
                    {"key": args.key, "value": args.value}))


def main():
    parser = argparse.ArgumentParser(
        description="Set up and drive a local CitrineOS for the charge point simulator.")
    parser.add_argument("--api", default="http://127.0.0.1:8080", help="REST API of CitrineOS")
    parser.add_argument("--tenant", type=int, default=1)
    parser.add_argument("--station", default=STATION, help="charge box identity")
    parser.add_argument("--db-container", default=DB_CONTAINER)
    parser.add_argument("--runtime", choices=("podman", "docker"),
                        help="container tool (default: whichever runs the database)")
    sub = parser.add_subparsers(dest="command", required=True)

    p = sub.add_parser("setup", help="station, connectors and identifiers")
    p.add_argument("--connectors", type=int, default=2)
    p.add_argument("--tag-prefix", default="ZEPHYR-")
    p.add_argument("--tags", nargs="*", help="identifiers instead of <prefix><connector>")
    p.set_defaults(func=cmd_setup)

    p = sub.add_parser("tag", help="add an identifier or change its status")
    p.add_argument("idtag")
    p.add_argument("--status", choices=TAG_STATUS, default="Accepted")
    p.set_defaults(func=cmd_tag)

    sub.add_parser("tags", help="identifiers").set_defaults(func=cmd_tags)
    sub.add_parser("stations", help="charging stations").set_defaults(func=cmd_stations)

    p = sub.add_parser("transactions", help="latest transactions")
    p.add_argument("--limit", type=int, default=10)
    p.set_defaults(func=cmd_transactions)

    p = sub.add_parser("start", help="RemoteStartTransaction")
    p.add_argument("connector", type=int)
    p.add_argument("idtag")
    p.set_defaults(func=cmd_start)

    p = sub.add_parser("stop", help="RemoteStopTransaction")
    p.add_argument("transaction", nargs="?", help="default: the active one of the station")
    p.set_defaults(func=cmd_stop)

    p = sub.add_parser("unlock", help="UnlockConnector")
    p.add_argument("connector", type=int)
    p.set_defaults(func=cmd_unlock)

    p = sub.add_parser("getconf", help="GetConfiguration")
    p.add_argument("key", nargs="*")
    p.set_defaults(func=cmd_getconf)

    p = sub.add_parser("setconf", help="ChangeConfiguration")
    p.add_argument("key")
    p.add_argument("value")
    p.set_defaults(func=cmd_setconf)

    args = parser.parse_args()
    args.func(Bench(args), args)


if __name__ == "__main__":
    main()
