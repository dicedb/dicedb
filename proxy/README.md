# DiceDB REST API Proxy

An initial REST API proxy for DiceDB written in C.

## Features

* **REST API:** Exposes DiceDB commands over a simple REST API interface (`/cmd/key/val`).
* **Valkey C SDK:** Uses the official Valkey C SDK (`hiredis`) for high performance and compatibility.
* **HTTP Server:** Powered by `libmicrohttpd` for lightweight, concurrent request handling.
* **Reactivity:** Server-Sent Events (SSE) backed by the DiceDB `OBSERVE` command.
* **Authentication:** Bearer tokens stored under `proxy:auth:tokens:<token>` in DiceDB, allowing tokens to be added and removed without restarting the proxy.
* **Rate limiting:** Shared fixed-window limits for each token and client IP.

This is not yet a complete implementation of issue #1787. Cluster-aware routing,
TLS, connection pooling, metrics, deployment manifests, and the full Upstash REST
API shape remain to be implemented.

## Building

Dependencies:
* `make`
* `gcc` / `clang`
* `libmicrohttpd` (e.g. `brew install libmicrohttpd` or `apt-get install libmicrohttpd-dev`)

To build the proxy:
```bash
make
```

Run the integration test with `make test` after building `../src/dicedb-server`.

## Running

```bash
./dicedb-proxy [port]
```

Configuration is read from `DICEDB_PROXY_PORT`, `DICEDB_HOST`, and
`DICEDB_PORT`. The defaults are `8080`, `127.0.0.1`, and `6379`.

## Examples

Start DiceDB:
```bash
docker run -p 6379:6379 dicedb/dicedb
```

Run the proxy:
```bash
./dicedb-proxy
```

Provision a bearer token:

```bash
../src/dicedb-cli SET proxy:auth:tokens:example-token active
```

Test the API:
```bash
curl -H 'Authorization: Bearer example-token' http://localhost:8080/SET/mykey/myval
curl -H 'Authorization: Bearer example-token' http://localhost:8080/GET/mykey
```
