# Spreadsheets

The server can export tables to a spreadsheet and import rows back. Two engines
are supported, chosen by `spreadsheet.engine`:

| Engine | Output | Dependency |
|---|---|---|
| `excel` (default) | `.xlsx` | `exceljs` (optional) |
| `csv` | `.csv` | none |

`exceljs` is an **optional** dependency, so `npm install` succeeds without it.
The `excel` engine reports what to do if it is missing rather than failing with
a module-not-found error:

> The "excel" engine needs exceljs. Run "npm install" or use --engine csv.

The CSV engine has no dependencies and exists so the tooling works on a machine
where the optional package was not installed.

## Export

```powershell
npm run db:export
```

Exports every table listed in `spreadsheet.tables` to `storage.spreadsheets`
(`./data/spreadsheets` by default).

### Options

| Flag | Effect |
|---|---|
| `--engine csv\|excel` | Override the configured engine |
| `--out <dir>` | Output directory |
| `--table <NAME>` | Export one table instead of all |
| `--config <path>` | Use a specific configuration file |

```powershell
# One table, as CSV
node scripts/export-spreadsheet.js --engine csv --out data/spreadsheets --table USERS

# Everything, as xlsx
npm run db:export
```

### What gets exported

The table list comes from `spreadsheet.tables` in configuration, not from the
schema, because an operator choosing what to export is making a reporting
decision:

```json
{
  "spreadsheet": {
    "engine": "excel",
    "defaultSheet": "MAINFRAME",
    "export": {
      "includeMetadataSheet": true,
      "freezeHeaderRow": true
    },
    "tables": [
      { "name": "USERS", "query": "SELECT * FROM users ORDER BY user_id" },
      { "name": "DATASETS", "query": "SELECT * FROM datasets ORDER BY dataset_id" },
      { "name": "JOBS", "query": "SELECT * FROM jobs ORDER BY job_id" },
      { "name": "TRANSACTIONS", "query": "SELECT * FROM transactions ORDER BY txn_id" },
      { "name": "AUDIT_LOG", "query": "SELECT * FROM audit_log ORDER BY log_id" }
    ]
  }
}
```

Each entry pairs a sheet name with the exact query that fills it, so a report
can order or filter without any code change.

With `includeMetadataSheet`, an extra sheet records the system name, sysplex,
export time and row counts. That sheet is what makes an exported file
self-describing - a spreadsheet found six months later still says where it came
from.

## Import

```powershell
node scripts/import-spreadsheet.js --file spreadsheets/USERS.csv --table users
```

| Flag | Effect |
|---|---|
| `--file <path>` | Source file (required) |
| `--table <name>` | Target table (required) |
| `--replace` | Delete existing rows before inserting |
| `--no-coerce` | Insert every value as text |
| `--config <path>` | Use a specific configuration file |

### Coercion

Values arrive from a spreadsheet as text and are converted to match the target
column's declared type: integers to numbers, empty cells to `NULL`, quoted
numbers left as text.

`--no-coerce` disables that, which is the right choice when a column is declared
`TEXT` but happens to contain digits - a dataset name like `1234` should stay a
string rather than becoming the number 1234.

### `--replace` is destructive and deliberate

The default appends. `--replace` empties the table first, and it is not the
default because importing a partially-filled file into a table you did not mean
to clear is not recoverable. Confirm the target before using it:

```powershell
node scripts/import-spreadsheet.js --file spreadsheets/USERS.csv --table users --replace
```

## Sample files

`spreadsheets/` ships a working sample per table, matching the seed data:

| File | Rows |
|---|---|
| `USERS.csv` | 2 |
| `DATASETS.csv` | 6 |
| `JOBS.csv` | 4 |
| `TRANSACTIONS.csv` | 6 |
| `VOLUMES.csv` | 3 |
| `SYSTEMS.csv` | 1 |
| `SYSTEM_PARAMETERS.csv` | 4 |

They double as the format reference: the header row names the columns, and the
importer matches by name rather than position, so column order does not matter.

### Round-tripping

```powershell
# Export, edit in a spreadsheet application, import back
node scripts/export-spreadsheet.js --engine csv --table USERS
#   ... edit data/spreadsheets/USERS.csv ...
node scripts/import-spreadsheet.js --file data/spreadsheets/USERS.csv --table users --replace
```

## Format notes

- **CSV** is written with `\r\n` line endings and every field quoted, so a
  value containing a comma, a quote or a newline survives the round trip.
  Quotes inside a value are doubled, per RFC 4180.
- **XLSX** applies the configured header style (`bold`, a fill colour, white
  text) and freezes the header row when `export.freezeHeaderRow` is set.
- Column order on export follows the query's `SELECT *`, which is schema order.
  On import it is ignored entirely.
- An import of a file whose headers do not match the table's columns will insert
  what it recognises and report what it did not, rather than failing the whole
  file. Check the summary line - it names the columns it could not map.

## Where files go

| Setting | Default | Used by |
|---|---|---|
| `storage.spreadsheets` | `./data/spreadsheets` | Export output |
| `spreadsheets/` | committed | Sample inputs |

`data/` is gitignored; `spreadsheets/` is committed. Exports are runtime
artifacts, samples are source.
