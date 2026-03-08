# OBSERVE Command Implementation Guide

## What OBSERVE Does

`OBSERVE <cmd> <key> [args...]` subscribes the client to real-time updates for the observed key:
- Sends an initial result immediately on subscription
- Re-evaluates and pushes results whenever the observed key changes
- Uses the existing pubsub infrastructure; events arrive on the same connection
- Respects `observe-debounce-period-ms` config to combine output of multiple updates and emit just once for debounce period

Only two top-level commands exist: `OBSERVE` and `UNOBSERVE`. No per-command variants like `GET.OBSERVE` are added.

## Response Format

All observe messages use a 5-element array:

```
["observe", "fingerprint", "<hex-fingerprint>", "result", <command-result>]
```

The fingerprint is a CRC64 hash of the command name + arguments (e.g., `GET key1`). Clients use it to match notifications to subscriptions.

## How Key Change Notifications Work

1. Any write command calls `signalModifiedKey(c->db, key)` in `src/db.c`
2. This calls `observeNotifyKeyChange(key, dbid)` in `src/observe.c`
3. If `observe_debounce_period > 0`, changes are buffered and flushed by a timer; otherwise fired immediately
4. `executeObserveCommand()` finds all fingerprints watching the key and pushes updates to subscribed clients

## Adding OBSERVE Support for a New Command

### Step 1 — Register the handler in observe.c

File: `src/observe.c`, function `findHandlerForCommand()` (near the top of the file):

```c
static observeCommandHandler findHandlerForCommand(const char *cmd_name) {
    if (!strcasecmp(cmd_name, "GET"))    return getCommand;
    if (!strcasecmp(cmd_name, "ZRANGE")) return zrangeCommand;
    if (!strcasecmp(cmd_name, "HGET"))   return hgetCommand;  /* ADD HERE */
    return NULL;
}
```

Rule: The handler must be the existing read-only command's proc function. It receives a client whose `argv[0]` is the command name and `argv[1]` is the key (same layout as a direct invocation).

### Step 2 — No other files needed

Because `OBSERVE` is a single generic command, no new command functions, `server.h` declarations, or `commands.def` entries are required for new supported commands.

## Key Files Reference

| File | What to change |
|------|----------------|
| `src/observe.c` | Add `else if` branch in `findHandlerForCommand()` |

## Existing Examples

- `OBSERVE GET key`: handled by `getCommand`, registered at `src/observe.c` in `findHandlerForCommand()`
- `OBSERVE ZRANGE key start stop [opts]`: handled by `zrangeCommand`

## Build, Run, and Test

### Build

```bash
cd ~/workspace/dicedb/dicedb
make
```

### Run

```bash
./src/dicedb-server
```

### Manual Test

```bash
# Terminal 1 — subscribe
./src/dicedb-cli
> OBSERVE GET user:name
# receives 5-element array immediately, then waits

# Terminal 2 — trigger update
./src/dicedb-cli
> SET user:name Alice
# Terminal 1 receives a new 5-element push message
```

### Integration Tests

```bash
# Run all observe tests
./runtest --single unit/observe

# Run a specific test
./runtest --single unit/observe --only "OBSERVE GET"
```

### Unit Tests (C)

```bash
cd src && make test
```

## Constraints

- Only readonly commands should be passed to `OBSERVE`
- In RESP2 mode, an observing connection is restricted to `OBSERVE`, `UNOBSERVE`, `PING`, `QUIT`, `RESET`
- In RESP3 mode, all commands work on the same connection
- Observing clients never time out (same as pubsub clients)
- `UNOBSERVE <fingerprint> [fingerprint ...]` removes subscriptions; returns count of removed subscriptions
