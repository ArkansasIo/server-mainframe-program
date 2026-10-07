-- ---------------------------------------------------------------------------
-- Mainframe Server System - schema
-- Dialect: SQLite (ANSI-compatible subset)
-- ---------------------------------------------------------------------------

PRAGMA foreign_keys = ON;

-- System images / LPARs -------------------------------------------------------
CREATE TABLE IF NOT EXISTS systems (
    system_id     INTEGER PRIMARY KEY AUTOINCREMENT,
    name          TEXT    NOT NULL UNIQUE,
    sysplex       TEXT    NOT NULL DEFAULT 'SYSPLEX-A',
    region        TEXT    NOT NULL DEFAULT 'DEFAULT',
    status        TEXT    NOT NULL DEFAULT 'ACTIVE'
                          CHECK (status IN ('ACTIVE','DRAINED','OFFLINE','FAILED')),
    version       TEXT    NOT NULL DEFAULT '1.0.0',
    created_at    TEXT    NOT NULL DEFAULT (datetime('now')),
    updated_at    TEXT    NOT NULL DEFAULT (datetime('now'))
);

-- Operator / end-user accounts -----------------------------------------------
CREATE TABLE IF NOT EXISTS users (
    user_id       INTEGER PRIMARY KEY AUTOINCREMENT,
    username      TEXT    NOT NULL UNIQUE,
    full_name     TEXT    NOT NULL DEFAULT '',
    department    TEXT    NOT NULL DEFAULT 'GENERAL',
    role          TEXT    NOT NULL DEFAULT 'OPERATOR'
                          CHECK (role IN ('GUEST','OPERATOR','ANALYST','ADMIN')),
    password_hash TEXT    NOT NULL DEFAULT '',
    status        TEXT    NOT NULL DEFAULT 'ACTIVE'
                          CHECK (status IN ('ACTIVE','LOCKED','DISABLED')),
    failed_logons INTEGER NOT NULL DEFAULT 0,
    last_logon    TEXT,
    created_at    TEXT    NOT NULL DEFAULT (datetime('now')),
    updated_at    TEXT    NOT NULL DEFAULT (datetime('now'))
);

CREATE INDEX IF NOT EXISTS idx_users_role   ON users(role);
CREATE INDEX IF NOT EXISTS idx_users_status ON users(status);

-- Datasets (catalogued record collections) -----------------------------------
CREATE TABLE IF NOT EXISTS datasets (
    dataset_id    INTEGER PRIMARY KEY AUTOINCREMENT,
    name          TEXT    NOT NULL UNIQUE,          -- e.g. MF1.PROD.CUSTOMER.MASTER
    dsorg         TEXT    NOT NULL DEFAULT 'PS'    -- PS | PO | VSAM
                          CHECK (dsorg IN ('PS','PO','VSAM','PDS')),
    recfm         TEXT    NOT NULL DEFAULT 'FB'    -- F | FB | VB | VBS | U
                          CHECK (recfm IN ('F','FB','VB','VBS','U')),
    lrecl         INTEGER NOT NULL DEFAULT 80,
    blksize       INTEGER NOT NULL DEFAULT 27920,
    volume        TEXT    NOT NULL DEFAULT 'MFVOL1',
    owner_user_id INTEGER,
    record_count  INTEGER NOT NULL DEFAULT 0,
    bytes_used    INTEGER NOT NULL DEFAULT 0,
    status        TEXT    NOT NULL DEFAULT 'AVAILABLE'
                          CHECK (status IN ('AVAILABLE','MIGRATED','DELETED')),
    created_at    TEXT    NOT NULL DEFAULT (datetime('now')),
    updated_at    TEXT    NOT NULL DEFAULT (datetime('now')),
    FOREIGN KEY (owner_user_id) REFERENCES users(user_id) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_datasets_owner  ON datasets(owner_user_id);
CREATE INDEX IF NOT EXISTS idx_datasets_status ON datasets(status);

-- Dataset records -------------------------------------------------------------
CREATE TABLE IF NOT EXISTS dataset_records (
    record_id   INTEGER PRIMARY KEY AUTOINCREMENT,
    dataset_id  INTEGER NOT NULL,
    sequence    INTEGER NOT NULL DEFAULT 0,
    payload     TEXT    NOT NULL DEFAULT '',
    created_at  TEXT    NOT NULL DEFAULT (datetime('now')),
    FOREIGN KEY (dataset_id) REFERENCES datasets(dataset_id) ON DELETE CASCADE
);

CREATE INDEX IF NOT EXISTS idx_records_dataset ON dataset_records(dataset_id, sequence);

-- Batch jobs ------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS jobs (
    job_id        INTEGER PRIMARY KEY AUTOINCREMENT,
    job_name      TEXT    NOT NULL,
    job_class     TEXT    NOT NULL DEFAULT 'A'
                          CHECK (job_class IN ('A','B','C','D','E')),
    step_name     TEXT    NOT NULL DEFAULT 'STEP0001',
    program       TEXT    NOT NULL DEFAULT 'IEFBR14',
    submitted_by  INTEGER,
    status        TEXT    NOT NULL DEFAULT 'QUEUED'
                          CHECK (status IN ('QUEUED','RUNNING','COMPLETE','ABEND','CANCELLED','HOLD')),
    return_code   INTEGER NOT NULL DEFAULT 0,
    priority      INTEGER NOT NULL DEFAULT 5,
    submitted_at  TEXT    NOT NULL DEFAULT (datetime('now')),
    started_at    TEXT,
    ended_at      TEXT,
    cpu_ms        INTEGER NOT NULL DEFAULT 0,
    output        TEXT    NOT NULL DEFAULT '',
    FOREIGN KEY (submitted_by) REFERENCES users(user_id) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_jobs_status    ON jobs(status);
CREATE INDEX IF NOT EXISTS idx_jobs_submitted ON jobs(submitted_at);

-- Transaction log (CICS-style) ------------------------------------------------
CREATE TABLE IF NOT EXISTS transactions (
    txn_id      INTEGER PRIMARY KEY AUTOINCREMENT,
    txn_code    TEXT    NOT NULL,                  -- e.g. CEMT, INQ1, UPDT
    terminal    TEXT    NOT NULL DEFAULT 'L3270',
    user_id     INTEGER,
    status      TEXT    NOT NULL DEFAULT 'OK'
                        CHECK (status IN ('OK','WARN','FAIL','ABEND')),
    rows_read   INTEGER NOT NULL DEFAULT 0,
    rows_written INTEGER NOT NULL DEFAULT 0,
    elapsed_ms  INTEGER NOT NULL DEFAULT 0,
    detail      TEXT    NOT NULL DEFAULT '',
    created_at  TEXT    NOT NULL DEFAULT (datetime('now')),
    FOREIGN KEY (user_id) REFERENCES users(user_id) ON DELETE SET NULL
);

CREATE INDEX IF NOT EXISTS idx_txn_code ON transactions(txn_code);
CREATE INDEX IF NOT EXISTS idx_txn_time ON transactions(created_at);

-- Audit log -------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS audit_log (
    log_id      INTEGER PRIMARY KEY AUTOINCREMENT,
    event_type  TEXT    NOT NULL,                  -- LOGON | LOGOFF | COMMAND | DB | CONFIG
    severity    TEXT    NOT NULL DEFAULT 'INFO'
                        CHECK (severity IN ('DEBUG','INFO','WARN','ERROR','CRITICAL')),
    actor       TEXT    NOT NULL DEFAULT 'SYSTEM',
    resource    TEXT    NOT NULL DEFAULT '',
    message     TEXT    NOT NULL DEFAULT '',
    metadata    TEXT    NOT NULL DEFAULT '{}',
    created_at  TEXT    NOT NULL DEFAULT (datetime('now'))
);

CREATE INDEX IF NOT EXISTS idx_audit_type ON audit_log(event_type);
CREATE INDEX IF NOT EXISTS idx_audit_time ON audit_log(created_at);

-- Storage volumes -------------------------------------------------------------
CREATE TABLE IF NOT EXISTS volumes (
    volume_id   INTEGER PRIMARY KEY AUTOINCREMENT,
    serial      TEXT    NOT NULL UNIQUE,
    device_type TEXT    NOT NULL DEFAULT '3390',
    capacity_mb INTEGER NOT NULL DEFAULT 1024,
    used_mb     INTEGER NOT NULL DEFAULT 0,
    status      TEXT    NOT NULL DEFAULT 'ONLINE'
                        CHECK (status IN ('ONLINE','OFFLINE','RESERVED','FULL')),
    created_at  TEXT    NOT NULL DEFAULT (datetime('now'))
);

-- Scheduler / system parameters -----------------------------------------------
CREATE TABLE IF NOT EXISTS system_parameters (
    param_key   TEXT PRIMARY KEY,
    param_value TEXT NOT NULL,
    description TEXT NOT NULL DEFAULT '',
    updated_at  TEXT NOT NULL DEFAULT (datetime('now'))
);
