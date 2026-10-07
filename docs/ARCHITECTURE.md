# Architecture

How the pieces fit together, and why they are shaped this way.

## The shape of the system

```
                      ┌──────────────────────────────────────┐
                      │            src/index.js              │
                      │   composition root: config, db,      │
                      │   then start the enabled services    │
                      └───────────────┬──────────────────────┘
                                      │
        ┌─────────────────────────────┼─────────────────────────────┐
        │                             │                             │
┌───────▼────────┐          ┌─────────▼─────────┐         ┌─────────▼────────┐
│  src/http/     │          │   src/tcp/        │         │   src/db/        │
│  control plane │          │  terminal gateway │         │  connection      │
│  :8080         │          │  :3270  :3271     │         │  migrate         │
└───────┬────────┘          └─────────┬─────────┘         └─────────┬────────┘
        │                             │                             │
        │                             │                             │
┌───────▼─────────────────────────────▼─────────────────────────────▼────────┐
│                            data/mainframe.db                               │
│                            (SQLite, WAL)                                   │
└────────────────────────────────────────────────────────────────────────────┘
```

Three listeners, one process, one database. There is no message bus and no
service discovery: a mainframe region is a single address space, and modelling
it as one process keeps the failure modes comprehensible.

## Why `src/index.js` is small

The entrypoint used to contain the HTTP server, the TCP gateway and the
dashboard template inline. Once the console grew a window manager, a view
registry and a rule engine's worth of configuration, the file was 456 lines
describing four unrelated things.

Now it is a **composition root**. It does exactly four things:

1. resolve configuration (`loadConfig`)
2. open and migrate the database (`initDatabase`)
3. start whichever services are enabled (`startServices`)
4. install signal handlers so Ctrl+C closes cleanly

Everything else lives in a module with one job. The test for whether this is
working is simple: reading `src/index.js` should tell you what the system is,
not how any part of it works.

## The layers

| Layer | Location | Responsibility |
|---|---|---|
| Configuration | `src/config/` | Layered merge, env overrides, CLI flags, validation |
| Database | `src/db/` | Connection, pragmas, migrations, repositories |
| Core | `src/core/` | Logger, audit, jobs, security primitives |
| Control plane | `src/http/` | REST endpoints, static assets, the dashboard page |
| Terminal | `src/tcp/` | Interactive gateway and the dataset transfer port |
| Presentation | `src/ui/`, `public/` | The console: components, views, keybinds, styling |

Dependencies point **downward only**. `src/http/` uses `src/core/logger` and the
database handle it is given; it never reaches into `src/tcp/`. The two protocol
layers are independent, which is what lets `--tcp-only` work without the HTTP
layer even loading.

## The control plane

Routes are declared as data in `src/http/server.js`:

```js
{ method: 'GET', path: '/api/rows', description: '...', handler }
```

rather than a chain of `if (url.pathname === ...)` tests. The chain worked at
four endpoints; it stops working when you want to answer questions *about* the
API - which endpoints exist, what a client may ask for - because that answer
ends up spread across a function body. The route table is also what
`describeRoutes()` reads for the docs, so the documentation cannot drift from
the implementation.

Handlers receive `(req, res, url)` and close over `{ config, db }`. Nothing
reaches for a global, so a route can be exercised in a test with a stub
database.

### Static assets have two roots

`public/` holds the console's own files. `src/ui/` holds the component library,
keybind registry and view registry - which must stay beside the source because
they are unit-tested in Node. `src/http/static.js` exposes them under bare names
(`/components.js`) so the browsing context and `require()` resolve the same
file rather than two copies that drift.

Path safety matters here: a request is resolved against a root and then checked
to still be inside it, so `/../src/index.js` cannot read arbitrary files. The
alias table is a fixed map rather than a pattern, which is what keeps the check
meaningful.

## The terminal layer

`src/tcp/terminal.js` is line-oriented: the client gets a banner and a prompt,
and submits a line at a time. That mirrors a 3270 session, where an operator
submits a screen rather than typing into a live cursor.

Commands are a table (`{ name, aliases, usage, description, run }`) and the
`HELP` reply is **generated from that table**. A command cannot exist without
appearing in help, and help cannot advertise something unwired. `run` receives a
per-connection context carrying the socket, config, database and logger.

`src/tcp/data.js` is the second port: a bulk record channel for moving a
dataset's records without occupying an interactive session. Its writes are
buffered until an explicit `COMMIT`, so an aborted upload cannot leave a
half-written dataset behind.

## The console

Split three ways so each part can be tested without the others:

- **`src/ui/components.js`** - reusable UI. Every component is a factory over a
  small `document`-like object rather than a class reaching for globals, which
  is what lets it run in Node.
- **`src/ui/views.js`** - the view registry. Nine views declared as data; the
  menu, palette and toolbar all read that one list, so a view cannot appear in
  the menu but be missing from the palette.
- **`public/dashboard.js`** - glue only. It owns the action table and wires the
  pieces together.

### The loads that break a browser

Three files loaded as `<script>` tags share **one top-level scope**. Two bugs
came from forgetting this:

- a bare `module.exports` is a `ReferenceError` in a browser
- a top-level `const el` in two files is a redeclaration

Both are covered by tests that load the real page in a browser-shaped VM. Any
file added to the bundle should be an IIFE.

## Data flow: opening a view

```
menu click / F10 / palette
        │
        ▼
  dispatch(action)                     dashboard.js - the one command surface
        │
        ▼
  openView(id) ──────────────► Views.createViewContext({ tables, health, ... })
        │                                 │
        │                                 ▼
        │                       view.build(context)  ── may throw
        │                                 │
        ▼                                 ▼
  windows.open(id, title, nodes)   Views.buildView() wraps the error
        │                          so one bad view cannot blank the console
        ▼
  createWindowPane → render, taskbar, z-order
```

An action name is the only coupling between the chrome and the behaviour. A
menu entry, a toolbar button, a palette row and a keybind all name the same
action; none of them contain behaviour of their own. That is what stops the
menu labels and the key map from disagreeing.

## Failure behaviour

| Failure | What happens |
|---|---|
| Bad configuration | `loadConfig` throws before anything binds a port |
| Missing database file | Created and migrated on first boot |
| A view throws in `build` | Error card in that window, console keeps working |
| An action has no handler | Warning toast naming the action |
| HTTP handler throws | 500 with JSON; the process stays up |
| Terminal command throws | `INTERNAL ERROR` on that socket only |
| Port already in use | `[http]`/`[tcp]` error logged, other services still start |

The boot loader follows the same rule: it advances only when real work
completes, and a failed step halts it with the reason on screen rather than
revealing a console that is not backed by a working service.
