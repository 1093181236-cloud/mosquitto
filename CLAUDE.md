# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A fork of Eclipse Mosquitto **v2.0.20** (fork point = upstream commit `a196c2b2`, tag `v2.0.20`), locally versioned as **2.0.23**, extended into an IoT-platform broker ("IoTEdgeDB" / luomi-iotp). Custom commits add an HTTP API to the broker, a device/gateway/time-series plugin, Windows porting, and a custom Docker image. Custom commit messages and `ChangeLog.txt` are written in Chinese (ChangeLog is GBK-encoded — preserve the encoding, don't re-save as UTF-8).

## Building

- **Linux (primary dev platform):** `make` from the repo root, configured by `config.mk` (`WITH_CJSON=yes` is required for the iedb plugin; `WITH_WEBSOCKETS=yes` for the HTTP API). `make binary` skips building the man pages (needed when building from git without docbook tools).
- **macOS (this machine):** the top-level `Makefile` hard-errors on Darwin ("Please compile using CMake on Mac OS X") — build with CMake:
  ```
  cmake . && make
  ```
- **Windows:** CMake + `vcpkg.json` (custom porting work; `#ifdef WIN32`/`_WIN32` branches throughout, including in `src/http.c` and `plugins/iedb/`).
- **Docker (Linux only):** `make iotpdocker` builds the `luomi-iotp:<version>` Alpine image from `docker/iotp/` (broker + `iedb.so` + pub/sub/rr/ctrl/passwd). Requires `lws.tar.gz` and `cjson.tar.gz` to be present in `docker/iotp/`. Stock images live under `docker/2.0/` etc.
- The iedb plugin links `-lcjson -lsqlite3`; cJSON, sqlite3, and OpenSSL are the main external deps.

## Tests

- `make test` / `make check` — full suite, serial (slow). `make ptest` — parallel (20 workers). `make utest` — CUnit unit tests.
- Run a single broker test: `cd test/broker && ./01-bad-initial-packets.py` — each `NN-*.py` file is a standalone test (Makefile groups them `01`…`14`). Client library tests are likewise standalone files under `test/lib/`.
- Tests need Python 3 and CUnit. Broker tests spin up a broker on localhost ports (1888+).

## Architecture — custom layer

**HTTP API in the broker** (`src/http.c` / `src/http.h`, wired into `src/websockets.c`): the websockets listener (port 8080 in `mosquitto-docker.conf`) also accepts plain HTTP GET/POST. `run_url()` parses the URI path into a command + `argc/argv` path segments (POST body appended as last arg) and dispatches to handlers registered via `url_register()` in a uthash table. Handlers return a status code and a cJSON response tree. `http_init()` registers the base `cpuid` and `licence` commands; the iedb plugin registers its own endpoints at init.

- **Licence mechanism** (`src/http.c`): machine-bound licensing — MD5 of CPUID, licence blob stored at `<persistence_location>/licence`, AES-128-CBC encrypted (key/IV are hardcoded string literals in `parser_licence()`; treat as intentional, flag rather than silently change). `check_licence()` currently has a hard `return 0;` stub — the licence gate is disabled.
- **Plugin ↔ broker symbol coupling:** the broker exports `url_register`/`url_unregister` to plugins via `src/linker.syms` (other symbols via `src/plugin_public.c`). The plugin calls the *broker's* HTTP router, not its own — plugins and broker must be built from matching sources.

**`iedb` plugin** (`plugins/iedb/` → `iedb.so`): a heavily extended fork of the upstream `dynamic-security` plugin. Adds on top of dynsec roles/groups/clients:
- Device registry: SQLite-backed (`db.c`), devices keyed by SN, held in a rax radix tree (`rax.c`, `device.c`) with tags (`tag.c` — tag queries support pagination).
- Time-series: `tsindexer.c` (point index), `tsrule.c` (aggregation rules), `tscompaction.c` (retention by hours/MB), LZF-compressed payloads (`lzf_c.c`/`lzf_d.c`).
- MQTT hooks (`mqtt.c`): basic-auth callback, message callback ingests realtime device data (topic prefix carries the gateway SN); if a reported device name is `system` it is rewritten to include the gateway SN. Also connect/disconnect tracking of MQTT clients (`mqtt_client` table).
- HTTP endpoints registered in `plugin.c`: `TSQUERY`, `TSREVQUERY`, `TSAGGQUERY`, `TSDIFFQUERY`, `TSLASTQUERY`, `TSBOOLQUERY`, `TSSCAN`, `TSADD/RULE`, `ALLDEVICES`, `ALLFIELDS`, `UPLOAD`, `NEWDEVICE`, `DELDEVICE`, plus device/tag/gateway commands.
- ACLs extended with `http_get` / `http_post` permissions that gate HTTP API access.
- Config: `plugin_opt_iedb_dir` (data dir), `plugin_opt_iedb_retention_hours` (default 24), `plugin_opt_iedb_retention_mbs` (default 100).
- Builds with `LOCAL_CPPFLAGS=-I../../src/` — it includes broker headers (`src/http.h`), which is why the Makefile has per-file rules for many objects.

**Config files** (three variants of the same broker config): `mosquitto.conf` (dev machine — contains hardcoded `/home/lighthouse/wft/...` paths for `plugin` and `plugin_opt_iedb_dir`; do not treat these as portable), `mosquitto-docker.conf` (image config: `listener 1883` mqtt, `listener 8080` websockets + `http_dir /mosquitto/www`, persistence to `/mosquitto/data`), `mosquitto-windows.conf` (Windows installs).

**Upstream parts unchanged:** broker core in `src/` (`handle_*.c`, `subs.c`, `retain.c`, `database.c` persistence), client library in `lib/`, C++ wrapper in `lib/cpp/`, clients in `client/`, apps (`mosquitto_passwd`, `mosquitto_ctrl`, `db_dump`), standard plugins in `plugins/` (`dynamic-security` is the upstream ancestor of `iedb`).

## Working with upstream

The upstream fork point is tag `v2.0.20` (commit `a196c2b2`); all tags ≤ v2.0.20 exist in the repo. `git diff v2.0.20..HEAD` shows the full custom surface (~80 files, ~20k lines, mostly `plugins/iedb/`). The custom work is concentrated in `src/http.c`, `src/websockets.c`, `src/linker.syms`, `plugins/iedb/`, the three conf files, and `docker/iotp/` — changes elsewhere are mostly incidental. `set-version.sh` is the version-bump script (version lives in `config.mk`, `include/mosquitto.h`, `CMakeLists.txt`, installer, snap, vcpkg).
