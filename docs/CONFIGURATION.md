# Configuration

## Resolution order

Later sources win. Each is deep-merged over the previous, so a file only needs
to state what it changes.

| # | Source | Notes |
|---|---|---|
| 1 | `config/default.json` | Committed base. Required. |
| 2 | `config/local.json` | Local overrides, or `--config <path>` |
| 3 | Environment variables | `MF_*`, mapped in `src/config/index.js` |
| 4 | CLI flags | `--port`, `--db`, `--log-level`, ... |

Arrays are **replaced**, not concatenated. An override that sets
`spreadsheet.tables` supplies the whole list; this is deliberate, because a
merged array of half-old and half-new table definitions is far harder to reason
about than an explicit list.

`config/local.json` is loaded automatically when present, which is why it is the
file to edit for local work. Passing `--config` bypasses it entirely.

## Environment variables

The full map from `src/config/index.js`:

### System

| Variable | Config path |
|---|---|
| `MF_SYSTEM_NAME` | `system.name` |
| `MF_SYSPLEX` | `system.sysplex` |
| `MF_REGION` | `system.region` |
| `MF_TIMEZONE` | `system.timezone` |
| `MF_MAX_JOBS` | `system.maxConcurrentJobs` |

### HTTP

| Variable | Config path |
|---|---|
| `MF_HTTP_ENABLED` | `http.enabled` |
| `MF_HTTP_HOST` | `http.host` |
| `MF_HTTP_PORT` | `http.port` |
| `MF_HTTP_BODY_LIMIT` | `http.bodyLimitBytes` |
| `MF_HTTP_AUTH_REQUIRED` | `http.auth.required` |
| `MF_API_KEYS` | `http.auth.apiKeys` |

`MF_API_KEYS` accepts a comma-separated list: `MF_API_KEYS=key1,key2`.

### Terminal

| Variable | Config path |
|---|---|
| `MF_TCP_ENABLED` | `tcp.enabled` |
| `MF_TCP_HOST` | `tcp.host` |
| `MF_TCP_TERMINAL_PORT` | `tcp.terminalPort` |
| `MF_TCP_DATA_PORT` | `tcp.dataPort` |

### Database

| Variable | Config path |
|---|---|
| `MF_DB_DRIVER` | `database.driver` (`sqlite` or `memory`) |
| `MF_DB_FILE` | `database.file` |
| `MF_DB_AUTO_MIGRATE` | `database.autoMigrate` |
| `MF_DB_AUTO_SEED` | `database.autoSeed` |

### Storage and spreadsheet

| Variable | Config path |
|---|---|
| `MF_SPREADSHEET_ENGINE` | `spreadsheet.engine` (`excel` or `csv`) |
| `MF_STORAGE_DATASETS` | `storage.datasets` |
| `MF_STORAGE_SPREADSHEETS` | `storage.spreadsheets` |
| `MF_STORAGE_UPLOADS` | `storage.uploads` |

### Logging

| Variable | Config path |
|---|---|
| `MF_LOG_LEVEL` | `logging.level` |
| `MF_LOG_FORMAT` | `logging.format` (`text` or `json`) |
| `MF_LOG_DIRECTORY` | `logging.directory` |
| `MF_LOG_CONSOLE` | `logging.console` |

### Security

| Variable | Config path |
|---|---|
| `MF_SESSION_TTL` | `security.session.ttlMinutes` |
| `MF_PASSWORD_MIN_LENGTH` | `security.passwordPolicy.minLength` |

### Value coercion

Environment values are strings, and are coerced before use:

| Input | Becomes |
|---|---|
| `true` / `false` | boolean |
| `null` | `null` |
| `8080` | integer |
| `1.5` | float |
| `[1,2,3]` | parsed JSON array |
| `a,b,c` (no spaces) | `['a','b','c']` |
| anything else | string, trimmed |

## CLI flags

| Flag | Effect |
|---|---|
| `--config <path>` | Layer this file instead of `config/local.json` |
| `--port <n>` | `http.port` |
| `--host <addr>` | `http.host` |
| `--terminal-port <n>` | `tcp.terminalPort` |
| `--data-port <n>` | `tcp.dataPort` |
| `--db <path>` | `database.file` |
| `--driver <name>` | `database.driver` |
| `--log-level <level>` | `logging.level` |
| `--log-format <fmt>` | `logging.format` |
| `--system-name <name>` | `system.name` |
| `--no-http` | Disable the control plane |
| `--no-tcp` | Disable both TCP listeners |
| `--tcp-only` | `--no-http`, for terminal-only operation |

## Validation

`loadConfig` validates the merged result and **throws** on a fatal problem
before any port is bound:

- `system.name` is required
- ports must be integers 0-65535
- `tcp.terminalPort` and `tcp.dataPort` must differ
- `database.driver` must be `sqlite` or `memory`
- `logging.level` must be one of trace/debug/info/warn/error
- `security.session.ttlMinutes` must be > 0

Some problems are warnings instead, because they are a legitimate configuration
rather than an error:

- `http.auth.required` is true but no API keys are set - every request will be
  rejected, which may be exactly what you want behind a proxy

## Worked examples

### Terminal-only, on a different port, with debug logging

```powershell
$env:MF_LOG_LEVEL = 'debug'
npm run tcp
```

### Move the database and keep everything else

```powershell
node src/index.js --db D:\mainframe\prod.db
```

### A one-off configuration file

```powershell
node src/index.js --config config\ci.json
```

### Check the resolved configuration without starting anything

```powershell
node -e "console.log(JSON.stringify(require('./src/config').loadConfig({argv:[]}), null, 2))"
```

The terminal's `CONFIG` command shows the resolved values for the running
system, including which file was the final source.

## The config schema

`config/schemas/config.schema.json` describes the shape for editors that
understand JSON Schema. It is documentation and editor support; the loader does
its own validation, so a schema mismatch will not silently change behaviour.

## Path resolution

Relative paths in configuration are resolved against the **project root**, not
the working directory, so the server behaves the same however it is launched.
This applies to `database.file`, the `storage.*` directories and
`logging.directory`.

`database.backup.directory` and the backup retention count are configured but
the backup job itself is not yet implemented - the settings are read and
reserved.
