-- ---------------------------------------------------------------------------
-- Maintenance / housekeeping statements
-- ---------------------------------------------------------------------------

-- name: purge_old_audit
DELETE FROM audit_log WHERE created_at < datetime('now', '-180 days');

-- name: purge_completed_jobs
DELETE FROM jobs WHERE status IN ('COMPLETE', 'CANCELLED') AND ended_at < datetime('now', '-30 days');

-- name: purge_old_transactions
DELETE FROM transactions WHERE created_at < datetime('now', '-90 days');

-- name: reclaim_deleted_datasets
DELETE FROM datasets WHERE status = 'DELETED';

-- name: vacuum
VACUUM;

-- name: analyze
ANALYZE;

-- name: integrity_check
PRAGMA integrity_check;
