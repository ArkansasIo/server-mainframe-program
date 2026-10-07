# Terminal

Two TCP listeners, separate from the HTTP control plane.

| Port | Default | Purpose |
|---|---|---|
| Terminal gateway | 3270 | Interactive, line-oriented commands |
| Dataset transfer | 3271 | Bulk record movement |

Both are configured under `tcp`:

```json
{
  "tcp": {
    "enabled": true,
    "host": "0.0.0.0",
    "terminalPort": 3270,
    "dataPort": 3271,
    "banner": "WELCOME TO MAINFRAME-1 - LOGON REQUIRED"
  }
}
```

`terminalPort` and `dataPort` must differ; the loader rejects a configuration
where they collide. Disable both with `--no-tcp`, or run only these with
`--tcp-only`.

## Why line-oriented

The gateway is not a raw byte pipe. The client receives a banner and a prompt,
and submits **one line at a time**, terminated by `\n`. Input is buffered
per-connection, so two operators cannot interleave their half-typed lines.

This mirrors a 3270 session, where an operator fills a screen and submits it,
rather than typing into a live cursor. It also makes the protocol drivable by
hand:

```powershell
telnet localhost 3270
```

## Commands

Commands are a table in `src/tcp/terminal.js`, and the `HELP` reply is
generated from that table. A command cannot exist without appearing in help, and
help cannot advertise something unwired.

| Command | Usage | Description |
|---|---|---|
| `HELP` | `HELP [command]` | List commands, or describe one. Alias: `?` |
| `STATUS` | `STATUS` | System identity, region, version, record counts, uptime |
| `LIST` | `LIST <table> [limit]` | Show rows (default 20, max 200) |
| `FIND` | `FIND <table> <text>` | Search every column of a table |
| `TABLES` | `TABLES` | List readable tables with row counts |
| `CONFIG` | `CONFIG` | The resolved runtime configuration |
| `LOGOFF` | `LOGOFF` | End the session. Aliases: `EXIT`, `QUIT`, `BYE` |

Commands are case-insensitive. Arguments may be quoted, so
`FIND datasets "MF1.PROD"` keeps the phrase together.

### Readable tables

```
users, datasets, jobs, transactions, audit_log, volumes
```

Each has a fixed column set chosen so a row fits an 80-column screen. That set
lives in `TABLE_COLUMNS` next to the commands.

### Example session

```
WELCOME TO MAINFRAME-LOCAL - LOGON REQUIRED
MAINFRAME-LOCAL / SYSPLEX-A - READY
TYPE HELP FOR COMMANDS
> STATUS
SYSTEM   : MAINFRAME-LOCAL
SYSPLEX  : SYSPLEX-A
REGION   : DEFAULT
VERSION  : 1.0.0

USERS         : 2
DATASETS      : 6
JOBS          : 4
TRANSACTIONS  : 6
AUDIT_LOG     : 5
VOLUMES       : 3

UPTIME   : 12 s
> LIST jobs 3
JOB_ID   JOB_NAME   JOB_CLASS  STATUS    RETURN_CODE SUBMITTED_AT
-------- ---------- ---------- --------- ----------- -------------------
1        PAYROLL    A          COMPLETE  0           2026-01-14 09:00:00
2        NIGHTLY    B          ABEND     12          2026-01-14 02:30:00
3        REPORT     C          QUEUED    0           2026-01-14 09:15:00

3 row(s) shown.
> LOGOFF
GOODBYE
```

Output uses `\r\n` line endings, the convention a telnet client expects.

## The dataset transfer port

`:3271` carries bulk records, so moving a dataset does not occupy an interactive
session. The protocol is self-describing and drivable by hand:

| Command | Reply | Meaning |
|---|---|---|
| `HELLO` | `OK <system> datasets=<n>` | Handshake |
| `LIST` | `DATASET <name> records=<n> lrecl=<n>` per row, then `END <n>` | Catalogue |
| `GET <name> [limit]` | `RECORD <seq> <payload>` per row, then `END <n>` | Read records |
| `PUT <name>` | `READY` | Begin a write |
| `DATA <payload>` | `STORED <n>` | Buffer one record |
| `COMMIT` | `COMMITTED <n> into <name>` | Write the buffer |
| `ABORT` | `ABORTED <n> record(s) discarded` | Discard the buffer |
| `QUIT` | `BYE` | Close |

Errors are prefixed `ERR`, so a client can distinguish a failure from data
without inspecting the command it sent.

### A transfer is buffered until COMMIT

`PUT` accumulates records in memory and only writes on `COMMIT`. An aborted or
dropped upload therefore cannot leave a half-written dataset behind - the whole
reason the protocol has an explicit commit rather than writing as it receives.

A transfer that goes quiet for 30 seconds (`SESSION_IDLE_MS`) is discarded on
the next activity, so an abandoned connection cannot hold a buffer forever. A
single `GET` is capped at 1000 records (`MAX_RECORDS_PER_GET`).

### Example transfer

```
> HELLO
OK MAINFRAME-LOCAL datasets=6
> PUT MF1.TEST.NEW
READY
> DATA first record
STORED 1
> DATA second record
STORED 2
> COMMIT
COMMITTED 2 into MF1.TEST.NEW
> GET MF1.TEST.NEW
RECORD 00000001 first record
RECORD 00000002 second record
END 2
> QUIT
BYE
```

`PUT` creates the dataset if it does not exist, appending to an existing one.

## Failure behaviour

| Failure | Result |
|---|---|
| Unknown command | `UNKNOWN COMMAND: <x>` then `TYPE HELP FOR A LIST`, session continues |
| Unknown table | `UNKNOWN TABLE: <x>` with the readable list |
| A command throws | `INTERNAL ERROR: <message>` on that socket only, session continues |
| Socket error | Logged at debug; other sessions unaffected |
| Port in use | Logged as `[tcp]`/`[tcp:data]` error; the HTTP service still starts |

One session's failure never takes down another, because each connection gets its
own context carrying the socket, config, database handle and logger.

## Security

There is **no authentication** on either port, and no TLS. Anyone who can reach
the port can read every readable table and write records through `:3271`.

This matches the original intent - a trusted-network terminal gateway - but it
is a real property to be aware of. Bind to `127.0.0.1` if the machine is not on
a trusted network:

```powershell
node src/index.js --host 127.0.0.1
```

Logon and session management are modelled by the `L`-prefixed tables and the
`[logon]` line in the seed data, but are not enforced.
