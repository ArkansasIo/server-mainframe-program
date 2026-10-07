# API

The HTTP control plane listens on `http.host:http.port` (default
`0.0.0.0:8080`). All responses are `application/json; charset=utf-8` unless
noted. Errors use one shape so a client never has to guess:

```json
{ "error": "Unknown resource: widgets", "status": 404 }
```

## Endpoints

| Method | Path | Description |
|---|---|---|
| GET | `/` | The operator console (HTML) |
| GET | `/index.html` | Alias of `/` |
| GET | `/api/health` | Liveness, identity, database state |
| GET | `/health` | Alias of `/api/health`, for infrastructure that has no idea about the `/api` prefix |
| GET | `/api/rows` | Rows from one table (`?table=&limit=`) |
| GET | `/api/tables` | Every table the API exposes |
| GET | `/api/menu` | The dashboard menu tree |

The route table lives in `src/http/server.js` as data, and
`describeRoutes(createRoutes(...))` produces the table above. The docs are
generated from the implementation, so they cannot silently drift.

## `GET /api/health`

```json
{
  "status": "ok",
  "system": "MAINFRAME-LOCAL",
  "sysplex": "SYSPLEX-A",
  "region": "DEFAULT",
  "version": "1.0.0",
  "database": { "driver": "sqlite", "users": 2 },
  "uptimeSeconds": 41
}
```

`status` is `"ok"` when the request is served at all - the process is up and
the route resolved. It does **not** assert that the database is writable. The
`database.users` count is read live, so a non-zero value is real evidence the
database answered.

The bare `/health` alias returns a smaller body: `status`, `system`,
`version`, `uptimeSeconds`. It exists for uptime checks that should not pay for
a query, and deliberately does not include the database section.

## `GET /api/rows`

Reads rows from a single table.

| Parameter | Default | Notes |
|---|---|---|
| `table` | - | Required. Must be in the allowed list. |
| `limit` | `500` | Clamped to 1-5000 |

```json
{
  "table": "users",
  "count": 2,
  "rows": [
    { "user_id": 1, "username": "OPERATOR", "role": "ADMIN", "status": "ACTIVE" }
  ]
}
```

An unknown or malformed table name is a `404` with the name echoed back:

```json
{ "error": "Unknown resource: widgets", "status": 404 }
```

### Allowed tables

Names come from `spreadsheet.tables` in configuration plus the operational
tables the console always wants:

```
audit_log, dataset_records, datasets, jobs, system_parameters,
transactions, users, volumes
```

A name must match `^[a-z][a-z0-9_]*$`. Anything else is rejected rather than
interpolated into SQL. Note that the configured names are used **as written** -
an earlier version stripped a trailing `s` as a singularisation heuristic, which
turned `USERS` into `user` (not a table) and made every request 404.

## `GET /api/tables`

```json
{ "tables": ["audit_log", "datasets", "jobs", "transactions", "users"] }
```

Useful for a client that wants to discover the data surface instead of
hard-coding it.

## `GET /api/menu`

Returns the console's menu tree, so a client can build its chrome from the
server rather than duplicating the structure.

```json
{
  "system": "MAINFRAME-LOCAL",
  "menus": [
    {
      "id": "file", "label": "File", "title": "File",
      "items": [
        { "id": "refresh", "label": "Refresh All", "action": "app.refresh" },
        { "label": "-" },
        { "id": "export", "label": "Export Panel", "action": "data.export" }
      ]
    }
  ]
}
```

A `"-"` entry is a separator. An entry with `children` is a submenu.

## Static assets

| Path | Served from |
|---|---|
| `/dashboard.js`, `/dashboard.css`, `/index.html`, `/favicon.svg` | `public/` |
| `/components.js`, `/keybinds.js`, `/views.js` | `src/ui/` |

The three `/src/ui/` files are exposed under bare names because they are
unit-tested in Node and must stay beside the source. Both the browsing context
and `require()` resolve the same file rather than two copies that drift.

Requests are resolved against a root and checked to still be inside it, so
`/../src/index.js` cannot read arbitrary files. Assets are sent with
`cache-control: no-cache` - the console is edited live, and a cached bundle is
a stale-bug generator.

## Status codes

| Code | Meaning |
|---|---|
| 200 | Success |
| 400 | Malformed URL, bad parameters, or a query the database rejected |
| 404 | No such endpoint, or a table that is not exposed |
| 500 | The handler threw. The message is logged; the response is generic. |

## Authentication

`http.auth` exists in configuration (`required`, `apiKeys`) and is validated -
if `required` is true with no keys, the loader warns that every request will be
rejected. **Enforcement is not yet wired into the request path**, so the control
plane is currently unauthenticated. Treat it as a trusted-network service, or
put it behind a proxy that authenticates.

## Examples

```powershell
# Liveness
curl.exe http://localhost:8080/api/health

# Ten newest-looking users
curl.exe "http://localhost:8080/api/rows?table=users&limit=10"

# What can I read?
curl.exe http://localhost:8080/api/tables
```

```javascript
// From the console, in the browser
await (await fetch('/api/rows?table=jobs')).json()
```

## Deliberate omissions

- **No writes.** The API is read-only. Writes go through the terminal gateway,
  where a command has a name and an audit line, rather than through an
  unauthenticated HTTP verb.
- **No pagination.** `limit` is a cap, not a cursor. At this scale the whole
  table fits in a response; adding cursors now would be speculative.
- **No compression.** Responses are small, and the terminal workload runs over
  a different port entirely.
