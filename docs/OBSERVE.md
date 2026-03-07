# OBSERVE Command Implementation Guide

## What OBSERVE Does

Every readonly command can have a `.OBSERVE` variant (e.g., `GET.OBSERVE`, `ZRANGE.OBSERVE`) that:
- Subscribes the client to real-time updates for the observed key
- Sends an initial result immediately on subscription
- Re-evaluates and pushes results whenever the observed key changes
- Uses the existing pubsub infrastructure; events arrive on the same connection
- Respects `observe-debounce-period-ms` config to combine output of multiple updates and emit just once for debounce period

## Response Format

All observe messages use a 5-element array:

```
[".observe", "fingerprint", "<hex-fingerprint>", "result", <command-result>]
```

The fingerprint is a CRC64 hash of the command name + arguments. Clients use it to match notifications to subscriptions.

## How Key Change Notifications Work

1. Any write command calls `signalModifiedKey(c->db, key)` in `src/db.c`
2. This calls `observeNotifyKeyChange(key, dbid)` in `src/observe.c`
3. If `observe_debounce_period > 0`, changes are buffered and flushed by a timer; otherwise fired immediately
4. `executeObserveCommand()` finds all fingerprints watching the key and pushes updates to subscribed clients

## Adding .OBSERVE Support for a New Command

### Step 1 — Add handler and command function in the module file

File: `src/t_<type>.c` (e.g., `src/t_hash.c` for HGET)

```c
/* HGET.OBSERVE handler - called both on subscribe and on key change */
void hgetObserveHandler(client *c) {
    hgetCommand(c);  /* reuse existing command implementation */
}

/* HGET.OBSERVE <key> <field> */
void hgetObserveCommand(client *c) {
    genericObserveCommand(c, hgetObserveHandler);
}
```

Rule: The handler must call the existing command (or its generic internal function) and produce output in `c`'s reply buffer. `genericObserveCommand` wraps it in the observe message format.

### Step 2 — Declare in server.h

File: `src/server.h` — add near other observe handler declarations (search for `getObserveHandler`):

```c
void hgetObserveHandler(client *c);
void hgetObserveCommand(client *c);
```

### Step 3 — Register handler in executeObserveCommand

File: `src/observe.c`, function `executeObserveCommand()` (line ~124):

```c
if (!strcasecmp(base_cmd, "GET")) {
    handler = getObserveHandler;
} else if (!strcasecmp(base_cmd, "ZRANGE")) {
    handler = zrangeObserveHandler;
} else if (!strcasecmp(base_cmd, "HGET")) {   /* ADD HERE */
    handler = hgetObserveHandler;
}
```

### Step 4 — Register command in commands.def

File: `src/commands.def` — add entry near the base command's definition. Copy the pattern from an existing observe entry:

```c
{MAKE_CMD("hget.observe","Observes HGET and streams updates","O(1)","8.0.0",
  CMD_DOC_NONE,NULL,NULL,"hash",COMMAND_GROUP_HASH,
  HGET_History,0,HGET_Tips,0,hgetObserveCommand,3,
  CMD_READONLY|CMD_FAST,ACL_CATEGORY_HASH,
  HGET_Keyspecs,1,NULL,2),.args=HGET_Args},
```

Parameters to adjust per command:
- `"hget.observe"` — dot-separated lowercase name
- Description string
- Complexity string
- `hgetObserveCommand` — the function from Step 1
- Arg count (include the command name itself)
- `CMD_READONLY` flags — keep readonly
- ACL category
- `HGET_Keyspecs` / `HGET_Args` — reuse from base command

## Key Files Reference

| File | What to change |
|------|----------------|
| `src/t_<type>.c` | Add `<cmd>ObserveHandler` and `<cmd>observeCommand` |
| `src/server.h` | Declare both functions |
| `src/observe.c` | Add `else if` branch in `executeObserveCommand()` |
| `src/commands.def` | Add `MAKE_CMD` entry |

## Existing Examples

- `GET.OBSERVE`: `src/t_string.c:356`, registered at `src/observe.c:126`
- `ZRANGE.OBSERVE`: `src/t_zset.c:3138`, registered at `src/observe.c:128`

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
> HGET.OBSERVE user:1 name
# receives 5-element array immediately, then waits

# Terminal 2 — trigger update
./src/dicedb-cli
> HSET user:1 name Alice
# Terminal 1 receives a new 5-element push message
```

### Integration Tests

```bash
# Run all observe tests
./runtest --single unit/observe

# Run a specific test
./runtest --single unit/observe --only "GET.OBSERVE"
```

### Unit Tests (C)

```bash
cd src && make test
```

## Constraints

- Only readonly commands should get `.OBSERVE` variants
- In RESP2 mode, an observing connection is restricted to `*.OBSERVE`, `UNOBSERVE`, `PING`, `QUIT`, `RESET`
- In RESP3 mode, all commands work on the same connection
- Observing clients never time out (same as pubsub clients)
- `UNOBSERVE <fingerprint> [fingerprint ...]` removes subscriptions; returns count of removed subscriptions
