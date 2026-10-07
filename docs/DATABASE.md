# Database

SQLite, one file, WAL mode. Chosen because a mainframe region is a single
address space with a single dataset catalogue - replicating that with a network
database would add a failure mode the model does not have.

## Connection

`src/db/connection.js` opens the file and applies pragmas from configuration:

```json
{
  "database": {
    "driver": "sqlite",
    "file": "./data/mainframe.db",
    "pragmas": {
      "journal_mode": "WAL",
      "foreign_keys": "ON",
      "busy_timeout": 5000
    }
  }
}
```

`foreign_keys = ON` is **per connection** in SQLite, not a property of the file.
Forgetting it silently disables every constraint, so it is applied on open
rather than assumed.

| Pragma | Effect |
|---|---|
| `journal_mode = WAL` | Readers do not block the writer |
| `foreign_keys = ON` | Constraints are enforced |
| `busy_timeout = 5000` | Wait 5s rather than failing instantly on a locked database |

### The `memory` driver

Setting `database.driver` to `memory` uses a small in-memory shim supporting
the subset of SQL this project issues. It exists so the server can boot without
the native binding - useful for tests and for a machine where `better-sqlite3`
will not compile. **It is not a database.** No DDL is tracked, `WHERE` supports
only `column = ?`, and `LIMIT` is honoured but nothing else is.

## Schema

Defined in `sql/schema.sql`. Everything uses `CREATE TABLE IF NOT EXISTS`, so
applying it twice is safe.

### Core tables

| Table | Holds |
|---|---|
| `users` | Operator accounts: role, status, failed logon count |
| `datasets` | The dataset catalogue: name, organisation, record format, volume |
| `dataset_records` | Records belonging to a dataset, ordered by `sequence` |
| `jobs` | Batch jobs: class, status, return code, timing |
| `transactions` | CICS-style transaction log |
| `audit_log` | Event trail: type, severity, actor, resource |
| `volumes` | Storage volumes and their capacity |
| `system_parameters` | Scheduler and system parameters |

### Constraints that carry meaning

Several columns are constrained to a fixed set rather than being free text, so a
typo is rejected at the point it is written rather than surfacing as a status
that no code path recognises:

| Column | Allowed values |
|---|---|
| `users.role` | `GUEST`, `OPERATOR`, `ANALYST`, `ADMIN` |
| `users.status` | `ACTIVE`, `LOCKED`, `DISABLED` |
| `datasets.dsorg` | `PS`, `PO`, `VSAM`, `PDS` |
| `datasets.recfm` | `F`, `FB`, `VB`, `VBS`, `U` |
| `datasets.status` | `AVAILABLE`, `MIGRATED`, `DELETED` |
| `jobs.job_class` | `A`-`E` |
| `jobs.status` | `QUEUED`, `RUNNING`, `COMPLETE`, `ABEND`, `CANCELLED`, `HOLD` |
| `transactions.status` | `OK`, `WARN`, `FAIL`, `ABEND` |
| `audit_log.severity` | `DEBUG`, `INFO`, `WARN`, `ERROR`, `CRITICAL` |
| `volumes.status` | `ONLINE`, `OFFLINE`, `RESERVED`, `FULL` |

### Relationships

```
users ─┬─< datasets.owner_user_id        ON DELETE SET NULL
       ├─< jobs.submitted_by             ON DELETE SET NULL
       └─< transactions.user_id          ON DELETE SET NULL

datasets ──< dataset_records.dataset_id
```

Foreign keys are `ON DELETE SET NULL` for ownership: removing an operator must
not delete the datasets they happened to create. The schema is designed so the
catalogue outlives the people who populated it.

## Migrations

`sql/migrations.sql` creates the tracking table. Revisions are applied by
`node scripts/db-init.js` in a fixed order:

| Revision | File | Contents |
|---|---|---|
| `001_schema` | `sql/schema.sql` | Tables and core indexes |
| `002_seed` | `sql/seed.sql` | Demonstration data |
| `003_views` | `sql/views.sql` | Reporting views |
| `004_indexes` | `sql/indexes.sql` | Performance indexes |
| `005_triggers` | `sql/triggers.sql` | Audit and integrity triggers |
| `006_views_ops` | `sql/views_ops.sql` | Operational reporting views |

Every file is written to be **idempotent**, so re-running is safe and a revision
that is already recorded is re-applied rather than skipped. That is a deliberate
trade: applying a `CREATE ... IF NOT EXISTS` twice costs nothing, whereas
skipping a file because a row exists in `schema_migrations` turns a half-applied
migration into a permanent inconsistency.

`schema_migrations` records what was applied and when:

```sql
SELECT revision, applied_at, description FROM schema_migrations ORDER BY revision;
```

### Indexes

Indexed columns are the ones actually filtered on: `users.role`,
`users.status`, `datasets.owner_user_id`, `datasets.status`, `jobs.status`,
`jobs.submitted_at`, `transactions.txn_code`, `transactions.created_at`,
`audit_log.event_type`, `audit_log.created_at`.

## Rebuilding

```powershell
npm run db:init              # create or migrate
node scripts/db-init.js --force   # delete and rebuild from scratch
```

`--force` removes the file and its `-wal` and `-shm` companions. Deleting only
the main file leaves stale WAL content that SQLite may try to replay, which is
why all three go together.

`db-init` also runs `PRAGMA foreign_key_check` after applying the revisions and
reports any violation, exiting non-zero if it finds one. A schema that loads
cleanly but violates its own constraints is worse than one that fails loudly.

## Seeding

```powershell
npm run db:seed
```

`sql/seed.sql` is idempotent - every statement is an `INSERT OR IGNORE` against
a unique key - so it is safe against a populated database. It ships:
2 operators, 6 datasets with records, 4 jobs (including one ABEND), 6
transactions, 3 volumes and a short audit trail.

## Backup settings

`database.backup` has `enabled`, `directory` and `retain`, and the loader
resolves the directory. **No backup job is implemented** - the settings are read
and reserved. A WAL-mode database should be backed up with SQLite's own
`.backup` rather than by copying the file, since a copy taken mid-write can
capture a torn state.

## Inspecting the database

```powershell
node scripts/db-query.js "SELECT username, role FROM users"
```

Or directly:

```powershell
node -e "const {loadConfig}=require('./src/config');const {openDatabase}=require('./src/db/connection');const c=loadConfig({argv:[]});const {db,close}=openDatabase(c,console);console.log(db.prepare('SELECT COUNT(*) n FROM users').get());close();"
```

## The HTTP read path

`GET /api/rows?table=<name>` reads through `allowedTables`, which intersects the
configured spreadsheet tables with a fixed operational list and validates each
name against `^[a-z][a-z0-9_]*$`. A name that fails that check is rejected
rather than interpolated into SQL.

Note the table name reaches the query by **string interpolation**, because SQL
cannot parameterise an identifier. The validation above is what makes that safe;
there is no path that reaches that line with an unvalidated name.
