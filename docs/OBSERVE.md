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

## Adding OBSERVE Support for a New Built-in Command

### Step 1 — Register the handler in observe.c

File: `src/observe.c`, function `findHandlerForCommand()`:

```c
static observeCommandHandler findHandlerForCommand(const char *cmd_name) {
    if (!strcasecmp(cmd_name, "GET"))    return getCommand;
    if (!strcasecmp(cmd_name, "ZRANGE")) return zrangeCommand;
    if (!strcasecmp(cmd_name, "HGET"))   return hgetCommand;  /* ADD HERE */
    /* ... */
    return NULL; /* falls through to module registry */
}
```

Rule: The handler must be the existing read-only command's proc function. It receives a client whose `argv[0]` is the command name and `argv[1]` is the key (same layout as a direct invocation).

### Step 2 — No other files needed

Because `OBSERVE` is a single generic command, no new command functions, `server.h` declarations, or `commands.def` entries are required for new supported commands.

## Adding OBSERVE Support for Module Commands

Modules can register their own read-only command handlers so that clients can `OBSERVE` them. The registration API in `src/observe.c` handles this without touching the built-in if/else chain.

### How it works

`findHandlerForCommand()` exhausts the built-in if/else table first. If nothing matches, it falls through to `server.observe_command_registry` — a case-insensitive dict populated via the registration API.

### Step 1 — Register on module load

Call `ValkeyModule_ObserveRegisterCommandHandler()` inside your module's `OnLoad` function:

```c
#include "server.h"

/* Your read-only command handler — same signature as any built-in command. */
void myModuleGetCommand(client *c) {
    /* c->argv[0] = command name, c->argv[1] = key, c->argv[2..] = extra args */
    robj *val = lookupKeyRead(c->db, c->argv[1]);
    if (val == NULL) {
        addReplyNull(c);
    } else {
        addReplyBulk(c, val);
    }
}

int ValkeyModule_OnLoad(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    if (ValkeyModule_Init(ctx, "mymodule", 1, VALKEYMODULE_APIVER_1) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    /* Register the command with Valkey as usual */
    if (ValkeyModule_CreateCommand(ctx, "MYMOD.GET", ...) == VALKEYMODULE_ERR)
        return VALKEYMODULE_ERR;

    /* Register the internal handler with OBSERVE so clients can subscribe */
    ValkeyModule_ObserveRegisterCommandHandler("MYMOD.GET", myModuleGetCommand);

    return VALKEYMODULE_OK;
}
```

After this, clients can run:

```
OBSERVE MYMOD.GET mykey
```

and receive push notifications whenever `mykey` changes.

### Step 2 — Unregister on module unload

Call `ValkeyModule_ObserveUnregisterCommandHandler()` in your `OnUnload` to prevent dangling handler pointers:

```c
int ValkeyModule_OnUnload(ValkeyModuleCtx *ctx) {
    ValkeyModule_ObserveUnregisterCommandHandler("MYMOD.GET");
    return VALKEYMODULE_OK;
}
```

### Step 3 — Signal key changes from write commands

OBSERVE fires when `signalModifiedKey` is called for a key. For built-in commands this happens automatically. For module write commands you must call it explicitly after mutating a key:

```c
void MyModuleSetCommand(ValkeyModuleCtx *ctx, ValkeyModuleString **argv, int argc) {
    /* ... write to your storage ... */

    /* Tell the server (and OBSERVE subscribers) that the key changed. */
    ValkeyModule_SignalModifiedKey(ctx, argv[1]);

    ValkeyModule_ReplyWithSimpleString(ctx, "OK");
}
```

Call this once per modified key, after the mutation is complete. If you skip it, any `OBSERVE` subscription on that key will not receive an update.

**When you do NOT need to call it:**

- You write via `ValkeyModule_Call(ctx, "SET"/"RESTORE"/…)` — the dispatched built-in command signals automatically.
- You opened the key with `ValkeyModule_OpenKey` in `WRITE` mode and closed it with `ValkeyModule_CloseKey` — the close path signals automatically (unless `VALKEYMODULE_OPTION_NO_IMPLICIT_SIGNAL_MODIFIED` is set).

Only use `ValkeyModule_SignalModifiedKey` when your write command bypasses both of the above paths (e.g. writing directly to an external store like RocksDB while keeping a Valkey key in sync).

### Handler contract

The handler registered via `ValkeyModule_ObserveRegisterCommandHandler()` must follow the same rules as any built-in OBSERVE handler:

| Rule | Details |
|------|---------|
| Read-only | Must not modify any key or global state |
| Signature | `ValkeyModuleObserveCmdProc` — `void (*)(struct client *c)`, identical to a built-in command proc |
| argv layout | `argv[0]` = command name, `argv[1]` = key, `argv[2..]` = additional args |
| Reply | Use `addReply*` as normal; the framework wraps the reply in the 5-element observe envelope |
| Key notifications | Call `ValkeyModule_SignalModifiedKey` from write commands that bypass the built-in key API; otherwise signalling is automatic |

### Module API reference

```c
/* Register (or update) a read handler so clients can OBSERVE your command.
 * Returns 1 if new, 0 if updated. */
int ValkeyModule_ObserveRegisterCommandHandler(const char *cmd_name, ValkeyModuleObserveCmdProc handler);

/* Remove a previously registered handler. No-op if not registered. */
void ValkeyModule_ObserveUnregisterCommandHandler(const char *cmd_name);

/* Signal that a key has been modified. Call this from write commands that
 * bypass ValkeyModule_OpenKey/CloseKey and ValkeyModule_Call, so that OBSERVE
 * subscribers receive their push notification. */
int ValkeyModule_SignalModifiedKey(ValkeyModuleCtx *ctx, ValkeyModuleString *keyname);
```

All three functions are declared in `src/valkeymodule.h` and implemented in `src/module.c`.

## Key Files Reference

| File | What to change |
|------|----------------|
| `src/observe.c` | Add branch in `findHandlerForCommand()` for built-in commands; `observeRegisterCommand` / `observeUnregisterCommand` for internal use |
| `src/server.h` | `observeRegisterCommand`, `observeUnregisterCommand`, `observeRegistryDictType` declared here (internal) |
| `src/valkeymodule.h` | `ValkeyModule_ObserveRegisterCommandHandler`, `ValkeyModule_ObserveUnregisterCommandHandler` — module-facing API |
| `src/module.c` | `VM_ObserveRegisterCommandHandler`, `VM_ObserveUnregisterCommandHandler` — bridge between module API and observe.c |

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
