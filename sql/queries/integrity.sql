-- ---------------------------------------------------------------------------
-- Mainframe Server System - integrity verification queries
--
-- These are the checks to run before and after any maintenance window, and
-- the ones the test suite asserts against. Every one of them should return
-- no rows on a healthy database.
--
-- Run one at a time, e.g.:
--   mf-db query --file sql/queries/integrity.sql
-- ---------------------------------------------------------------------------

-- name: check_foreign_keys
-- Any foreign-key violation anywhere in the schema. SQLite reports every
-- breach as (table, rowid, parent, constraint index).
PRAGMA foreign_key_check;

-- name: check_orphaned_records
-- Dataset records with no dataset. Should be empty: the FK is ON DELETE
-- CASCADE, so any row here means a writer disabled foreign keys.
SELECT
    r.record_id                                         AS record_id,
    r.dataset_id                                        AS dataset_id,
    r.sequence                                          AS sequence,
    r.created_at                                        AS created_at
FROM dataset_records r
LEFT JOIN datasets d ON d.dataset_id = r.dataset_id
WHERE d.dataset_id IS NULL
ORDER BY r.dataset_id, r.sequence;

-- name: check_counter_drift
-- Datasets whose stored counters disagree with their records.
SELECT
    d.dataset_id                                        AS dataset_id,
    d.name                                              AS name,
    d.record_count                                      AS stored_records,
    COALESCE(a.records, 0)                              AS actual_records,
    d.bytes_used                                        AS stored_bytes,
    COALESCE(a.bytes, 0)                                AS actual_bytes
FROM datasets d
LEFT JOIN (
    SELECT dataset_id, COUNT(*) AS records, SUM(LENGTH(payload)) AS bytes
    FROM dataset_records GROUP BY dataset_id
) a ON a.dataset_id = d.dataset_id
WHERE d.record_count <> COALESCE(a.records, 0)
   OR d.bytes_used   <> COALESCE(a.bytes, 0);

-- name: check_sequence_gaps
-- Duplicated or out-of-order record sequences within a dataset.
SELECT
    r.dataset_id                                        AS dataset_id,
    r.sequence                                          AS sequence,
    COUNT(*)                                            AS occurrences
FROM dataset_records r
GROUP BY r.dataset_id, r.sequence
HAVING COUNT(*) > 1
ORDER BY r.dataset_id, r.sequence;

-- name: check_volume_drift
-- Volumes whose used_mb disagrees with the datasets placed on them.
SELECT
    v.serial                                            AS volume,
    v.used_mb                                           AS stored_used_mb,
    COALESCE(COALESCE(SUM(d.bytes_used), 0) / 1048576, 0) AS actual_used_mb,
    v.capacity_mb                                       AS capacity_mb,
    v.status                                            AS status
FROM volumes v
LEFT JOIN datasets d ON d.volume = v.serial AND d.status <> 'DELETED'
GROUP BY v.volume_id
HAVING v.used_mb <> COALESCE(COALESCE(SUM(d.bytes_used), 0) / 1048576, 0);

-- name: check_jobs_without_owner
-- Submitted jobs whose account has since been deleted. The FK sets NULL, so
-- this is informational rather than an error - but it is worth knowing.
SELECT
    j.job_id                                            AS job_id,
    j.job_name                                          AS job_name,
    j.status                                            AS status,
    j.submitted_at                                      AS submitted_at,
    j.return_code                                       AS return_code
FROM jobs j
WHERE j.submitted_by IS NULL
ORDER BY j.submitted_at DESC;

-- name: check_stuck_jobs
-- Running or queued jobs that have not moved for over a day. The scheduler
-- should never leave one of these behind.
SELECT
    j.job_id                                            AS job_id,
    j.job_name                                          AS job_name,
    j.job_class                                         AS job_class,
    j.status                                            AS status,
    j.submitted_at                                      AS submitted_at,
    j.started_at                                        AS started_at,
    CAST(julianday('now') - julianday(j.submitted_at) AS INTEGER) AS days_queued
FROM jobs j
WHERE j.status IN ('QUEUED', 'RUNNING', 'HOLD')
  AND j.submitted_at < datetime('now', '-1 day')
ORDER BY j.submitted_at;

-- name: check_abnormal_logons
-- Accounts with a rising failed-logon count - possible password attack.
SELECT
    u.username                                          AS username,
    u.role                                              AS role,
    u.status                                            AS status,
    u.failed_logons                                     AS failed_logons,
    u.last_logon                                        AS last_logon
FROM users u
WHERE u.failed_logons > 0
ORDER BY u.failed_logons DESC;

-- name: check_never_used_accounts
-- Active accounts that have never logged on. Review before an audit.
SELECT
    u.username                                          AS username,
    u.role                                              AS role,
    u.department                                        AS department,
    u.created_at                                        AS created_at
FROM users u
WHERE u.last_logon IS NULL
  AND u.status = 'ACTIVE'
ORDER BY u.created_at;

-- name: check_datasets_on_offline_volumes
-- A dataset that lives on a volume that is not ONLINE. The allocator should
-- have migrated these away.
SELECT
    d.name                                              AS name,
    d.volume                                            AS volume,
    v.status                                            AS volume_status,
    d.status                                            AS dataset_status,
    d.bytes_used                                        AS bytes_used
FROM datasets d
JOIN volumes v ON v.serial = d.volume
WHERE v.status <> 'ONLINE'
  AND d.status = 'AVAILABLE'
ORDER BY d.volume, d.name;

-- name: check_unacknowledged_criticals
-- Every CRITICAL event still outstanding. Should be acknowledged before the
-- shift ends.
SELECT
    a.log_id                                            AS log_id,
    a.created_at                                        AS created_at,
    a.actor                                             AS actor,
    a.resource                                          AS resource,
    a.message                                           AS message
FROM audit_log a
WHERE a.severity = 'CRITICAL'
ORDER BY a.log_id DESC;

-- name: repair_counters
-- Not a check - the repair. Recompute record_count and bytes_used for every
-- dataset from the records themselves. Run after check_counter_drift has
-- reported rows, or after bulk-loading with the triggers disabled.
UPDATE datasets
   SET record_count = COALESCE((
           SELECT COUNT(*) FROM dataset_records r
            WHERE r.dataset_id = datasets.dataset_id), 0),
       bytes_used = COALESCE((
           SELECT SUM(LENGTH(r.payload)) FROM dataset_records r
            WHERE r.dataset_id = datasets.dataset_id), 0);

-- name: repair_volume_usage
-- Recompute every volume's used_mb from its datasets, then reclassify FULL.
--
-- Written as one statement: SQLite's prepare() accepts a single statement, so
-- two UPDATEs under one name fail with "contains more than one statement".
-- The status is derived from the freshly computed total rather than from the
-- stored used_mb, so a single pass both corrects the usage and reclassifies.
UPDATE volumes
   SET used_mb = (
           SELECT COALESCE(SUM(d.bytes_used) / 1048576, 0) FROM datasets d
            WHERE d.volume = volumes.serial AND d.status <> 'DELETED'),
       status = CASE
           WHEN (SELECT COALESCE(SUM(d.bytes_used) / 1048576, 0) FROM datasets d
                  WHERE d.volume = volumes.serial AND d.status <> 'DELETED')
                >= volumes.capacity_mb
                AND volumes.status = 'ONLINE' THEN 'FULL'
           WHEN (SELECT COALESCE(SUM(d.bytes_used) / 1048576, 0) FROM datasets d
                  WHERE d.volume = volumes.serial AND d.status <> 'DELETED')
                <  volumes.capacity_mb
                AND volumes.status = 'FULL'   THEN 'ONLINE'
           ELSE volumes.status
       END;
