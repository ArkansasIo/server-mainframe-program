# Mainframe Server System

A self-contained mainframe-style server: an HTTP control plane, a TN3270-style
TCP terminal gateway, a dataset (record) transfer port, a relational database
layer, a layered configuration system, and SQL + spreadsheet (Excel/CSV) tooling.

```
server-mainframe/
├── config/                  # configuration files + JSON schema
│   ├── default.json         # base configuration (committed)
│   ├── local.json           # local overrides (safe to edit)
│   └── schemas/             # config.schema.json
├── src/
│   ├── index.js             # entrypoint (boots http + tcp + db)
│   ├── config/              # config loader, env overrides, validation
│   ├── db/                  # connection, migrations, repositories
│   ├── core/                # jobs, datasets, security, audit, logger
│   ├── http/                # REST control plane + static dashboard
│   ├── tcp/                 # terminal gateway + dataset transfer
│   └── util/                # small helpers
├── sql/
│   ├── schema.sql           # DDL
│   ├── seed.sql             # seed data
│   ├── views.sql            # reporting views
│   └── queries/             # ad-hoc reporting SQL
├── spreadsheets/            # sample CSV spreadsheets
├── scripts/                 # CLI: db init/seed/backup/query, launch, spreadsheets
│   ├── launch.js            # boot: check deps, prepare db, start, open browser
│   ├── db-init.js           # create/migrate the database
│   ├── db-seed.js           # re-apply the sample data
│   ├── db-backup.js         # consistent snapshot (VACUUM INTO) + verify
│   ├── db-query.js          # run the SQL in sql/queries/
│   ├── check-queries.js     # run every query against the schema
│   └── export|import-spreadsheet.js
├── docs/                    # architecture, config, api, database, terminal, sheets
├── public/                  # operator dashboard (static)
├── tests/                   # node:test suites
├── data/                    # runtime (gitignored)
└── logs/                    # runtime (gitignored)
```

## Quick start

```powershell
npm install
npm run db:init      # create the database from sql/schema.sql + seed
npm start            # boot HTTP (:8080) and TCP terminal (:3270)
```

Then open <http://localhost:8080/> for the operator dashboard.

## Spreadsheets

```powershell
npm run db:export                       # export all configured tables -> .xlsx
node scripts/export-spreadsheet.js --engine csv --out data/spreadsheets
node scripts/import-spreadsheet.js --file spreadsheets/USERS.csv --table users
```

## Database maintenance

```powershell
npm run db:init          # create or migrate from sql/schema.sql
npm run db:seed          # re-apply the sample data
npm run db:backup        # consistent snapshot -> data/backups
npm run db:backups       # list existing snapshots
npm run db:query         # run a query, e.g. --sql "SELECT 1"
npm run db:check-queries # run every query in sql/queries against the schema
```

`db:query` reads by default. The repair statements in `sql/queries/integrity.sql`
modify data and require `--write`, so looking at a repair cannot apply it.

## Configuration

Resolution order (later wins):

1. `config/default.json`
2. `config/local.json` (or `--config <path>`)
3. Environment variables (`MF_HTTP_PORT`, `MF_DB_FILE`, `MF_LOG_LEVEL`, ...)
4. CLI flags (`--port`, `--db`, `--log-level`)

See `src/config/index.js` for the full environment-variable mapping and
`docs/CONFIGURATION.md` for details.

## Documentation

- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)
- [docs/CONFIGURATION.md](docs/CONFIGURATION.md)
- [docs/API.md](docs/API.md)
- [docs/DATABASE.md](docs/DATABASE.md)
- [docs/TERMINAL.md](docs/TERMINAL.md)
- [docs/SPREADSHEETS.md](docs/SPREADSHEETS.md)

## License

MIT
