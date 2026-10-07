-- ---------------------------------------------------------------------------
-- Mainframe Server System - performance indexes
--
-- The core indexes live in sql/schema.sql next to the tables they belong to.
-- This file adds the secondary indexes the reporting and maintenance queries
-- in sql/queries/ actually rely on, plus the covering indexes that keep the
-- dashboard refresh cheap on a large catalog.
--
-- Every statement is CREATE INDEX IF NOT EXISTS, so this file is idempotent
-- and safe to re-run after a schema change.
-- ---------------------------------------------------------------------------

PRAGMA foreign_keys = ON;

-- ---------------------------------------------------------------------------
-- users
-- ---------------------------------------------------------------------------
-- Sorting the operator list and the "who has not logged on" report.
CREATE INDEX IF NOT EXISTS idx_users_username      ON users(username);
CREATE INDEX IF NOT EXISTS idx_users_last_logon    ON users(last_logon);
-- The lockout path increments failed_logons; a partial index keeps it small.
CREATE INDEX IF NOT EXISTS idx_users_failed_logons ON users(failed_logons)
    WHERE failed_logons > 0;

-- ---------------------------------------------------------------------------
-- datasets
-- ---------------------------------------------------------------------------
-- Catalog searches use LIKE '%NAME%' and ORDER BY name; this index serves the
-- ordering half without the engine having to sort.
CREATE INDEX IF NOT EXISTS idx_datasets_name       ON datasets(name);
CREATE INDEX IF NOT EXISTS idx_datasets_volume     ON datasets(volume);
-- The dashboard counts AVAILABLE datasets only, so index that subset.
CREATE INDEX IF NOT EXISTS idx_datasets_available  ON datasets(status, bytes_used DESC)
    WHERE status = 'AVAILABLE';
-- Covering index for the size report: the query needs no table access.
CREATE INDEX IF NOT EXISTS idx_datasets_bytes      ON datasets(bytes_used DESC, name, volume);

-- ---------------------------------------------------------------------------
-- dataset_records
-- ---------------------------------------------------------------------------
-- The listing query is "WHERE dataset_id = ? ORDER BY sequence", which the
-- composite index in schema.sql already covers. This adds the reverse scan
-- used when reading the newest record of a dataset.
CREATE INDEX IF NOT EXISTS idx_records_sequence    ON dataset_records(dataset_id, sequence DESC);
CREATE INDEX IF NOT EXISTS idx_records_created     ON dataset_records(created_at);

-- ---------------------------------------------------------------------------
-- jobs
-- ---------------------------------------------------------------------------
-- Queue depth: status IN (...) ORDER BY priority DESC, submitted_at ASC.
CREATE INDEX IF NOT EXISTS idx_jobs_queue          ON jobs(status, priority DESC, submitted_at)
    WHERE status IN ('QUEUED', 'RUNNING', 'HOLD');
CREATE INDEX IF NOT EXISTS idx_jobs_class          ON jobs(job_class);
CREATE INDEX IF NOT EXISTS idx_jobs_submitter      ON jobs(submitted_by);
-- Purge scans ended_at for terminal states.
CREATE INDEX IF NOT EXISTS idx_jobs_ended          ON jobs(ended_at)
    WHERE ended_at IS NOT NULL;

-- ---------------------------------------------------------------------------
-- transactions
-- ---------------------------------------------------------------------------
CREATE INDEX IF NOT EXISTS idx_txn_status          ON transactions(status);
CREATE INDEX IF NOT EXISTS idx_txn_user            ON transactions(user_id);
CREATE INDEX IF NOT EXISTS idx_txn_terminal        ON transactions(terminal);
-- Failure triage: the only rows the operator acts on.
CREATE INDEX IF NOT EXISTS idx_txn_failures        ON transactions(created_at DESC)
    WHERE status IN ('FAIL', 'ABEND');
-- Activity report groups by code; this makes the GROUP BY an index scan.
CREATE INDEX IF NOT EXISTS idx_txn_code_time       ON transactions(txn_code, created_at DESC);

-- ---------------------------------------------------------------------------
-- audit_log
-- ---------------------------------------------------------------------------
CREATE INDEX IF NOT EXISTS idx_audit_actor         ON audit_log(actor);
CREATE INDEX IF NOT EXISTS idx_audit_severity      ON audit_log(severity);
CREATE INDEX IF NOT EXISTS idx_audit_unack         ON audit_log(created_at DESC)
    WHERE severity IN ('WARN', 'ERROR', 'CRITICAL');
-- Retention purge walks created_at from the oldest end.
CREATE INDEX IF NOT EXISTS idx_audit_retention     ON audit_log(created_at);

-- ---------------------------------------------------------------------------
-- volumes
-- ---------------------------------------------------------------------------
CREATE INDEX IF NOT EXISTS idx_volumes_status      ON volumes(status);
CREATE INDEX IF NOT EXISTS idx_volumes_used        ON volumes(used_mb DESC);

-- ---------------------------------------------------------------------------
-- systems
-- ---------------------------------------------------------------------------
CREATE INDEX IF NOT EXISTS idx_systems_sysplex     ON systems(sysplex);
CREATE INDEX IF NOT EXISTS idx_systems_status      ON systems(status);

-- ---------------------------------------------------------------------------
-- system_parameters
-- ---------------------------------------------------------------------------
-- param_key is the primary key, so lookups are already covered. This supports
-- the "show me everything in a JES2 or RACF group" query pattern.
CREATE INDEX IF NOT EXISTS idx_params_group        ON system_parameters(param_key);

-- ---------------------------------------------------------------------------
-- Snapshot the query planner's view so a regression is easy to spot.
-- ---------------------------------------------------------------------------
ANALYZE;
